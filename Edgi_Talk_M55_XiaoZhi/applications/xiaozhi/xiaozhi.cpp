/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-12-14     RT-Thread    First version
 */

#include "xiaozhi.h"
#include "xiaozhi_port.h"
#include "../screen_saver.h"
#include "xz_server_cfg.h"     /* 服务端地址/端口/TLS 开关的运行时读取 */
#include <lwip/apps/websocket_client.h>
#include <cJSON.h>
#include <netdev.h>
#include <webclient.h>
#include <wavplayer.h>
#include <string.h>
#include <lwip/ip4_addr.h>     /* ip4addr_aton：连接自建服务端时跳过多余 DNS */

#define DBG_TAG "xz.ws"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

/* Configuration constants */
#define MAX_CLIENT_ID_LEN 40
#define MAX_MAC_ADDR_LEN 20
#define WEBSOCKET_RECONNECT_DELAY_MS 5000
#define WEBSOCKET_CONNECTION_TIMEOUT_MS 5000
#define NETWORK_CHECK_DELAY_MS 500
#define RETRY_DELAY_BASE_MS 1000
#define RETRY_DELAY_INCREMENT_MS 200
#define TTS_STOP_DELAY_MS 500
#define BUTTON_DEBOUNCE_MS 20
#define WAKEWORD_INIT_FLAG_RESET 0
#define TTS_SENTENCE_TIMEOUT_MS 6000

/* Global application state */
xiaozhi_app_t xz_app =
    {
        .xiaozhi_tid = RT_NULL,
        .client_id = "af7ac552-9991-4b31-b660-683b210ae95f",
        .websocket_reconnect_flag = 0,
        .iot_initialized = 0,
        .last_reconnect_time = 0,
        .mac_address_string = {0},
        .client_id_string = {0},
        .ws = {0},
        .state = kDeviceStateUnknown,
        .button_event = RT_NULL,
        /* 多轮自动续听默认关闭。
         *
         * 开启时，AI 说完话会自动进入下一轮聆听（两条路径：tts 的
         * sentence_end 超时兜底，以及 TTS stop 后的 500ms 延迟重启），
         * 表现为"未按侧键，麦克风自行开启"。这在纯语音助手场景属于特性，
         * 但本工程采用按住说话交互：麦克风仅在用户按住侧键期间开启。
         *
         * 关闭后，每轮对话都必须由按键发起，麦克风的开合与侧键严格对应。
         * 如需恢复连续对话，将其改回 RT_TRUE 即可；相关逻辑（三处判断）
         * 均保留，未删除。 */
        .multi_turn_conversation_enabled = RT_FALSE,
        .tts_sentence_end_timer = RT_NULL,
        .tts_stop_workqueue = RT_NULL,
        .pending_listen_start = RT_FALSE,
        .exit_req = RT_FALSE};

#include "ui/xiaozhi_ui.h"
#include "iot/iot_c_api.h"
#include "mcp/mcp_api.h"


/* TTS sentence end timeout handler */
static void tts_sentence_end_timeout(void *parameter)
{
    uint32_t tick = rt_tick_get();
    LOG_D("TTS sentence end timeout at tick=%u, sending event to restart listening for multi-turn conversation", tick);

    /* Send event to button thread to handle the restart in non-ISR context */
    rt_err_t ev_ret = rt_event_send(xz_app.button_event, TIMEOUT_EVENT);
    if (ev_ret != RT_EOK)
    {
        LOG_W("rt_event_send returned %d from timer callback", ev_ret);
    }
}

/* TTS stop delayed restart work function */
static void tts_stop_restart_listening(struct rt_work *work, void *work_data)
{
    LOG_D("TTS stop delayed restart: restarting listening mode");

    xz_app.state = kDeviceStateListening;
    xz_mic(1);

    /* Try to send listen start */
    if (ws_send_listen_start(&xz_app.ws.clnt, xz_app.ws.session_id, kListeningModeAutoStop))
    {
        xiaozhi_ui_chat_status("   聆听中");
        xiaozhi_ui_chat_output("聆听中...");
    }
    else
    {
        LOG_W("Listen start failed in TTS stop delayed restart");
        /* Reset state if failed */
        xz_app.state = kDeviceStateIdle;
        xz_mic(0);
        xiaozhi_ui_chat_status("   就绪");
        xiaozhi_ui_chat_output("就绪");
    }
}

/* Static work item for TTS stop restart */
static struct rt_work tts_stop_work;

/* Static timer for TTS stop delay */
static rt_timer_t tts_stop_delay_timer = RT_NULL;

/* TTS stop delay timer callback */
static void tts_stop_delay_timeout(void *parameter)
{
    LOG_D("TTS stop delay timeout, submitting work to restart listening");

    /* Submit work to restart listening */
    if (xz_app.tts_stop_workqueue)
    {
        rt_workqueue_submit_work(xz_app.tts_stop_workqueue, &tts_stop_work, 0);
    }
    else
    {
        LOG_W("Workqueue not available in timer callback");
        /* Fallback */
        tts_stop_restart_listening(&tts_stop_work, RT_NULL);
    }
}

/* State consistency check function */
static void ensure_state_consistency(void)
{
    /* If listening but disconnected, handle multi-turn logic */
    if (xz_app.state == kDeviceStateListening && !xz_app.ws.is_connected)
    {
        LOG_W("Inconsistent state detected: Listening but disconnected, fixing...\n");
        xz_mic(0);

        if (xz_app.multi_turn_conversation_enabled)
        {
            LOG_D("Multi-turn conversation enabled, attempting to reconnect after disconnection");
            xz_app.state = kDeviceStateIdle;
            xiaozhi_ui_chat_status("   连接中");
            xiaozhi_ui_chat_output("重新连接中...");
            reconnect_websocket();
        }
        else
        {
            xz_app.state = kDeviceStateIdle;
            xiaozhi_ui_chat_status("   就绪");
            xiaozhi_ui_chat_output("就绪");

        }
    }

    /* If speaking but disconnected, reset too */
    if (xz_app.state == kDeviceStateSpeaking && !xz_app.ws.is_connected)
    {
        LOG_W("Inconsistent state detected: Speaking but disconnected, fixing...\n");
        xz_speaker(0);
        xz_mic(0);
        xz_app.state = kDeviceStateUnknown;
        xiaozhi_ui_chat_status("   休眠中");
        xiaozhi_ui_chat_output("等待唤醒");

    }

    /* Optimize: if connected but state unknown, set to idle */
    if (xz_app.state == kDeviceStateUnknown && xz_app.ws.is_connected && xz_app.ws.session_id[0] != '\0')
    {
        LOG_D("WebSocket connected with valid session, updating state to Idle\n");
        xz_app.state = kDeviceStateIdle;
        xiaozhi_ui_chat_status("   就绪");
        xiaozhi_ui_chat_output("就绪");

    }
}

/* Helper functions */
char *get_mac_address(void)
{
    struct netdev *netdev = netdev_get_by_name("w0");
    if (netdev == RT_NULL)
    {
        LOG_E("Cannot find netdev w0");
        return "";
    }

    if (netdev->hwaddr_len != 6)
    {
        LOG_E("Invalid MAC address length: %d", netdev->hwaddr_len);
        return "";
    }

    rt_snprintf(xz_app.mac_address_string, sizeof(xz_app.mac_address_string),
                "%02x:%02x:%02x:%02x:%02x:%02x",
                netdev->hwaddr[0], netdev->hwaddr[1], netdev->hwaddr[2],
                netdev->hwaddr[3], netdev->hwaddr[4], netdev->hwaddr[5]);
    return xz_app.mac_address_string;
}

char *get_client_id(void)
{
    if (xz_app.client_id_string[0] == '\0')
    {
        uint32_t tick = rt_tick_get();
        uint8_t hash_input[64];
        uint8_t hash_output[32];
        int input_len = rt_snprintf((char *)hash_input, sizeof(hash_input),
                                    "%s%u", xz_app.client_id, tick);

        for (int i = 0; i < 32; i++)
        {
            hash_output[i] = (uint8_t)(hash_input[i % input_len] ^ (tick >> (i % 32)));
        }

        int j = 0;
        for (int i = 0; i < 16; i++)
        {
            if (i == 4 || i == 6 || i == 8 || i == 10)
            {
                xz_app.client_id_string[j++] = '-';
            }
            xz_app.client_id_string[j++] = "0123456789abcdef"[hash_output[i] >> 4];
            xz_app.client_id_string[j++] = "0123456789abcdef"[hash_output[i] & 0xF];
        }
        xz_app.client_id_string[j] = '\0';
        LOG_D("Generated Client ID: %s", xz_app.client_id_string);
    }
    return xz_app.client_id_string;
}

void xz_button_callback(void *arg)
{
    if (CYBSP_BTN_PRESSED == Cy_GPIO_Read(CYBSP_USER_BTN_PORT, CYBSP_USER_BTN_PIN))
    {
        rt_event_send(xz_app.button_event, BUTTON_EVENT_PRESSED);
        screen_saver_notify_activity();
    }
#ifndef ExBoard_Voice
    else
    {
        rt_event_send(xz_app.button_event, BUTTON_EVENT_RELEASED);
        screen_saver_notify_activity();
    }
#endif
}

/* 触摸"按住说话"：将事件投递到与实体按键相同的事件组。
 *
 * 不直接调用 xz_mic 或 ws_send_listen_start 的原因：状态机全部位于
 * event_thread 中（负责等待连接、打断播报、重连中忽略、manual_stop 收尾），
 * 绕过状态机自行实现会导致两处状态不一致。此处只负责"模拟一次按键"，
 * 后续行为与实体键完全一致。
 *
 * 注意 event_thread 由 xz_ws_audio_init() 创建，事件组也在该函数中创建。
 * 界面在 START 之前即可点击该按钮，因此必须处理 NULL：返回 -1 让 UI
 * 提示"先启动"，而不是向空指针投递。
 *
 * rt_event_send 可在任意线程调用（内部为关中断修改标志位并解锁调度器），
 * 在 LVGL 线程中调用是安全的。 */
extern "C" int xz_touch_ptt(rt_bool_t pressed)
{
    if (xz_app.button_event == RT_NULL)
        return -1;

    if (pressed)
    {
        rt_event_send(xz_app.button_event, BUTTON_EVENT_PRESSED);
    }
    else
    {
        /* 松手事件仅在确实按下过之后才有意义。新建会话时若物理键与
         * 触摸键均未按下，多余的 RELEASED 会使状态机打印
         * "not listening, nothing to stop"，虽无危害但会污染日志。 */
        rt_event_send(xz_app.button_event, BUTTON_EVENT_RELEASED);
    }

    screen_saver_notify_activity();
    return 0;
}

void xz_event_thread_entry(void *param)
{
    rt_uint32_t evt;
    while (1)
    {
        rt_event_recv(xz_app.button_event,
                      BUTTON_EVENT_PRESSED | BUTTON_EVENT_RELEASED | TIMEOUT_EVENT,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &evt);

        /* First ensure state consistency */
        ensure_state_consistency();

        if (evt & BUTTON_EVENT_PRESSED)
        {
            if (!xz_app.ws.is_connected)
            {
                xz_app.pending_listen_start = RT_TRUE;

                /* Check if reconnecting */
                if (xz_app.websocket_reconnect_flag == 1)
                {
                    LOG_D("Reconnection already in progress, ignoring button press\n");
                    xiaozhi_ui_chat_status("   连接中");
                    xiaozhi_ui_chat_output("仍在连接中...");
                    continue;
                }

                LOG_D("Device not connected, initiating wake up...\n");
                xiaozhi_ui_chat_status("   连接中");
                xiaozhi_ui_chat_output("正在连接小智...");
                reconnect_websocket();
            }
            else
            {
                /* WebSocket connected, can handle requests */
                /* If unknown state but connected, treat as idle */
                if (xz_app.state == kDeviceStateSpeaking)
                {
                    LOG_D("Speaking aborted by user\n");
                    xz_speaker(0);
                }

                /* Press-to-talk: check if already listening */
                if (xz_app.state != kDeviceStateListening)
                {
                    LOG_D("Starting listening mode - press once to talk\n");

                    xz_app.pending_listen_start = RT_FALSE;

                    xiaozhi_ui_chat_status("   聆听中");
                    xiaozhi_ui_chat_output("聆听中...");
                    /* 手动停止模式：由松手事件决定录音何时结束。
                     *
                     * 原先使用 auto_stop，由服务端 VAD 判定静音后收尾。
                     * 这与按住说话的交互矛盾：环境存在背景音时 VAD
                     * 无法判定静音，松手后麦克风仍在录音。
                     * manual_stop 模式下服务端不做自动收尾，等待本地发送的
                     * listen stop（见 BUTTON_EVENT_RELEASED 分支）。 */
                    xz_mic(1);
                    ws_send_listen_start(&xz_app.ws.clnt, xz_app.ws.session_id,
                                         kListeningModeManualStop);
                }
                else
                {
                    LOG_D("Already in listening mode\n");
                }
            }
        }
        else if (evt & BUTTON_EVENT_RELEASED)
        {
            /* 按住说话：松手即停止，使服务端立即开始识别。
             *
             * 原代码在此不做任何处理（注释为 "no need to stop on release,
             * auto-handle"），配合 listen_start 使用的 auto_stop 模式，由
             * 服务端 VAD 判定静音后收尾。真机表现为松手后仍在录音：
             * 环境存在背景音时 VAD 无法及时判定静音，麦克风持续开启，
             * 用户会认为按键未生效。
             *
             * 发送 listen stop 后，服务端 listenMessageHandler 收到
             * state=="stop" 会立即用已缓冲的音频触发 ASR，响应也更快。
             *
             * 仅在确实处于 Listening 状态时发送：手指按到一半断连，或服务端
             * 已自行收尾时补一个 stop，会让服务端拿空缓冲跑一次 ASR。 */
            if (xz_app.state == kDeviceStateListening)
            {
                LOG_D("Button released - stopping listening (manual stop)\n");

                xz_mic(0);
                ws_send_listen_stop(&xz_app.ws.clnt, xz_app.ws.session_id);
            }
            else
            {
                LOG_D("Button released - not listening, nothing to stop\n");
            }
        }
        else if (evt & TIMEOUT_EVENT)
        {
            /* Handle TTS sentence end timeout for multi-turn conversation */
            LOG_I("Processing TTS sentence end timeout event");

            /* Only restart listening if we're still in speaking state, multi-turn is enabled, and connected */
            if (xz_app.state == kDeviceStateSpeaking && xz_app.multi_turn_conversation_enabled && xz_app.ws.is_connected)
            {
                xz_app.state = kDeviceStateListening;
                xz_mic(1);

                /* Try to send listen start */
                if (ws_send_listen_start(&xz_app.ws.clnt, xz_app.ws.session_id, kListeningModeAutoStop))
                {
                    xiaozhi_ui_chat_status("   聆听中");
                    xiaozhi_ui_chat_output("聆听中...");
                    LOG_D("Listen start successful after sentence end timeout");
                }
                else
                {
                    LOG_W("Listen start failed after sentence end timeout");
                    /* Reset state if failed */
                    xz_app.state = kDeviceStateIdle;
                    xz_mic(0);
                }
            }
            else if (!xz_app.ws.is_connected)
            {
                LOG_D("Skipping timeout restart - WebSocket disconnected");
            }
        }
    }
}

void xz_button_init(void)
{
    rt_pin_mode(BUTTON_PIN, PIN_MODE_INPUT_PULLUP);
    rt_pin_write(BUTTON_PIN, PIN_HIGH);
    xz_app.button_event = rt_event_create("btn_evt", RT_IPC_FLAG_FIFO);
    RT_ASSERT(xz_app.button_event != RT_NULL);

    rt_pin_attach_irq(BUTTON_PIN, PIN_IRQ_MODE_RISING_FALLING,
                      xz_button_callback, NULL);
    rt_pin_irq_enable(BUTTON_PIN, RT_TRUE);
    LOG_D("[Init] Button handler ready\n");
}

void xz_event_init(void)
{
    rt_thread_t tid = rt_thread_create("event_thread",
                                       xz_event_thread_entry,
                                       RT_NULL, 3 * 1024, 7, 10);
    RT_ASSERT(tid != RT_NULL);
    if (rt_thread_startup(tid) != RT_EOK)
    {
        LOG_E("Button thread startup failed\n");
        return;
    }
}

/* WebSocket communication functions */
rt_bool_t ws_send_listen_start(void *ws, char *session_id, enum ListeningMode mode)
{
    static const char *mode_str[] = {"auto_stop", "manual_stop", "always_on"};
    static char message[256];
    rt_snprintf(message, 256,
                "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"start\","
                "\"mode\":\"%s\"}",
                session_id, mode_str[mode]);

    if (xz_app.ws.is_connected && xz_app.ws.ws_write_mutex)
    {
        if (rt_mutex_take(xz_app.ws.ws_write_mutex, RT_WAITING_NO) == RT_EOK)
        {
            if (xz_app.ws.is_connected)
            {
                err_t result = wsock_write((wsock_state_t *)ws, message, strlen(message), OPCODE_TEXT);
                LOG_D("ws_send_listen_start result: %d\n", result);
                if (result == ERR_OK)
                {
                    /* Update state only on success, keep sync */
                    xz_app.state = kDeviceStateListening;
                    LOG_D("State updated to Listening after successful send\n");
                    rt_mutex_release(xz_app.ws.ws_write_mutex);
                    return RT_TRUE;
                }
                else
                {
                    LOG_E("Failed to send listen start message: %d\n", result);
                    if (result == ERR_CLSD || result == ERR_RST)
                    {
                        xz_app.ws.is_connected = 0;
                    }
                    rt_mutex_release(xz_app.ws.ws_write_mutex);
                    return RT_FALSE;
                }
            }
            rt_mutex_release(xz_app.ws.ws_write_mutex);
            return RT_FALSE;
        }
        else
        {
            LOG_D("WebSocket write busy, cannot send listen start\n");
            return RT_FALSE;
        }
    }
    else
    {
        LOG_E("WebSocket not connected, cannot send listen start\n");
        return RT_FALSE;
    }
}

void ws_send_listen_stop(void *ws, char *session_id)
{
    static char message[256];
    rt_snprintf(message, 256,
                "{\"session_id\":\"%s\",\"type\":\"listen\",\"state\":\"stop\"}",
                session_id);

    if (xz_app.ws.is_connected && xz_app.ws.ws_write_mutex)
    {
        if (rt_mutex_take(xz_app.ws.ws_write_mutex, RT_WAITING_NO) == RT_EOK)
        {
            if (xz_app.ws.is_connected)
            {
                err_t result = wsock_write((wsock_state_t *)ws, message, strlen(message), OPCODE_TEXT);
                LOG_D("ws_send_listen_stop result: %d\n", result);
                if (result == ERR_OK)
                {
                    /* Update state only on success, keep sync */
                    xz_app.state = kDeviceStateIdle;
                    LOG_D("State updated to Idle after successful send\n");

                }
                else
                {
                    LOG_E("Failed to send listen stop message: %d\n", result);
                    if (result == ERR_CLSD || result == ERR_RST)
                    {
                        xz_app.ws.is_connected = 0;
                    }
                }
            }
            rt_mutex_release(xz_app.ws.ws_write_mutex);
        }
        else
        {
            LOG_D("WebSocket write busy, cannot send listen stop\n");
        }
    }
    else
    {
        LOG_D("WebSocket not connected, cannot send listen stop\n");
        /* Even if disconnected, ensure correct state */
        xz_app.state = kDeviceStateIdle;
    }
}

void ws_send_hello(void *ws)
{
    if (xz_app.ws.is_connected && xz_app.ws.ws_write_mutex)
    {
        if (rt_mutex_take(xz_app.ws.ws_write_mutex, RT_WAITING_NO) == RT_EOK)
        {
            if (xz_app.ws.is_connected)
            {
                wsock_write((wsock_state_t *)ws, HELLO_MESSAGE,
                            strlen(HELLO_MESSAGE), OPCODE_TEXT);
            }
            rt_mutex_release(xz_app.ws.ws_write_mutex);
        }
    }
    else
    {
        /* 未连接，直接丢弃本帧 */
    }
}

void xz_audio_send_using_websocket(uint8_t *data, int len)
{
    if (xz_app.ws.is_connected)
    {
        // Get mutex to prevent concurrent writes
        if (xz_app.ws.ws_write_mutex && rt_mutex_take(xz_app.ws.ws_write_mutex, RT_WAITING_NO) == RT_EOK)
        {
            // Check connection again, prevent disconnect during lock wait
            if (xz_app.ws.is_connected)
            {
                err_t err = wsock_write(&xz_app.ws.clnt, (const char *)data, len, OPCODE_BINARY);
                if (err != ERR_OK)
                {
                    LOG_D("wsock_write failed: %d, connection may be closing\n", err);
                    // Write failed, mark connection as disconnected
                    if (err == ERR_CLSD || err == ERR_RST)
                    {
                        xz_app.ws.is_connected = 0;
                    }
                }
            }
            rt_mutex_release(xz_app.ws.ws_write_mutex);
        }
        else
        {
            // Cannot get lock or not initialized, skip this send
            LOG_D("WebSocket write busy, skip audio data\n");
        }
    }
}

err_t my_wsapp_fn(int code, char *buf, size_t len)
{
    switch (code)
    {
    case WS_CONNECT:
        LOG_D("websocket connected\n");
        if ((uint16_t)(uint32_t)buf == 101)
        {
            xz_app.ws.is_connected = 1;
            rt_sem_release(xz_app.ws.sem);
        }
        break;
    case WS_DISCONNECT:
        // Get write lock to ensure no ongoing writes
        if (xz_app.ws.ws_write_mutex)
        {
            rt_mutex_take(xz_app.ws.ws_write_mutex, RT_WAITING_FOREVER);
        }

        /* Ignore disconnect callback during reconnection to avoid state confusion */
        if (xz_app.websocket_reconnect_flag == 1)
        {
            LOG_D("Ignore disconnect during reconnect\n");
            if (xz_app.ws.ws_write_mutex)
            {
                rt_mutex_release(xz_app.ws.ws_write_mutex);
            }
            break;
        }
        /* Ignore disconnect callback if already disconnected */
        if (!xz_app.ws.is_connected)
        {
            LOG_D("Ignore disconnect when not connected\n");
            if (xz_app.ws.ws_write_mutex)
            {
                rt_mutex_release(xz_app.ws.ws_write_mutex);
            }
            break;
        }

        /* Stop mic and speaker on disconnect */
        if (xz_app.state == kDeviceStateListening)
        {
            xz_mic(0);
            LOG_D("Stopped microphone recording due to disconnection\n");
        }
        else if (xz_app.state == kDeviceStateSpeaking)
        {
            xz_speaker(0);
            LOG_D("Stopped speaker due to disconnection\n");
        }

        /* Ensure mic is closed when entering sleep */
        xz_mic(0);

        /* Stop TTS sentence end timer if running */
        if (xz_app.tts_sentence_end_timer)
        {
            rt_err_t stop_ret = rt_timer_stop(xz_app.tts_sentence_end_timer);
            LOG_D("rt_timer_stop returned %d when stopping timer due to disconnection", stop_ret);
            if (stop_ret == RT_EOK)
            {
                LOG_D("Stopped TTS sentence end timer due to disconnection");
            }
        }

        xiaozhi_ui_chat_status("   休眠中");
        xiaozhi_ui_chat_output("等待唤醒");
        LOG_I("WebSocket closed\n");
        xz_app.ws.is_connected = 0;
        xz_app.state = kDeviceStateUnknown;

        /* Release write lock */
        if (xz_app.ws.ws_write_mutex)
        {
            rt_mutex_release(xz_app.ws.ws_write_mutex);
        }

        /* Clear reconnect timestamp for immediate retry */
        xz_app.last_reconnect_time = 0;
        break;
    case WS_TEXT:
        Message_handle((const uint8_t *)buf, len);
        break;
    case WS_DATA:
        /* TTS 下行音频：按需求不播放，直接丢弃。
         *
         * 不影响对话的原因：AI 的文字回复走完全独立的路径，服务端发送
         * type="tts" 的文本帧，state="sentence_start" 时带 text 字段
         * （见 Message_handle 的 tts 分支），该路径照常进入聊天气泡。
         * 此处的二进制帧只是同一句话的 Opus 音频。
         *
         * 节省的资源：256 个下行队列缓冲（每块 256B）、Opus 解码器，
         * 以及扬声器设备的数据通路。录音上行（Opus encoder）不受影响。
         *
         * 如需恢复播放：改回 xz_audio_downlink(...)，并让 xz_sound_init()
         * 重新 rt_device_open 扬声器设备。 */
        break;
    default:
        LOG_E("Unknown error\n");
        break;
    }
    return 0;
}

void reconnect_websocket(void)
{
    err_t result;
    uint32_t retry = 10;
    uint32_t current_time = rt_tick_get();

    /* Prevent frequent reconnects: at least 5s since last */
    if (xz_app.websocket_reconnect_flag == 1)
    {
        LOG_D("Reconnection already in progress, ignoring duplicate request\n");
        return;
    }

    if (current_time - xz_app.last_reconnect_time < rt_tick_from_millisecond(WEBSOCKET_RECONNECT_DELAY_MS))
    {
        LOG_D("Reconnection too frequent, ignoring request\n");
        return;
    }

    xz_app.last_reconnect_time = current_time;

    /* Set reconnect flag to ignore disconnect callbacks during reconnection */
    xz_app.websocket_reconnect_flag = 1;

    while (retry-- > 0)
    {
        /* Check WebSocket TCP state, avoid reconnect at bad times */
        if (xz_app.ws.clnt.pcb != RT_NULL)
        {
            LOG_D("WebSocket PCB exists, current state: %d\n", xz_app.ws.clnt.pcb->state);

            /* Clean up only when state is abnormal */
            if (((struct tcp_pcb *)xz_app.ws.clnt.pcb)->state != CLOSED && ((struct tcp_pcb *)xz_app.ws.clnt.pcb)->state != CLOSE_WAIT)
            {
                LOG_I("Cleaning up WebSocket connection in state %d\n", xz_app.ws.clnt.pcb->state);

                /* Reset connection flag first, avoid reconnect callback interference */
                xz_app.ws.is_connected = 0;

                /* Try normal close */
                err_t close_result = wsock_close(&xz_app.ws.clnt, WSOCK_RESULT_LOCAL_ABORT, ERR_OK);
                LOG_D("wsock_close result: %d\n", close_result);

                /* Give system time to clean up resources - reduced from 2000ms */
                rt_thread_mdelay(500);

                /* Check if closed successfully */
                if (xz_app.ws.clnt.pcb != RT_NULL && ((struct tcp_pcb *)xz_app.ws.clnt.pcb)->state != CLOSED)
                {
                    LOG_W("WebSocket PCB still exists after close, forcing cleanup\n");
                    /* Force cleanup - reduced delay */
                    rt_thread_mdelay(100);
                    memset(&xz_app.ws.clnt, 0, sizeof(wsock_state_t));
                }
            }
        }

        /* Ensure connection state reset */
        xz_app.ws.is_connected = 0;

        if (!xz_app.ws.sem)
        {
            xz_app.ws.sem = rt_sem_create("xz_ws", 0, RT_IPC_FLAG_FIFO);
        }
        else
        {
            /* Reset semaphore to avoid stale signals */
            while (rt_sem_trytake(xz_app.ws.sem) == RT_EOK)
                ;
        }

        char *client_id = get_client_id();

        /* Ensure WebSocket struct fully cleaned */
        memset(&xz_app.ws.clnt, 0, sizeof(wsock_state_t));

        /* 连接参数全部运行时取（见 xz_server_cfg.h）：
         *   wsock_init 的第 2 个参数是 ssl_enabled。原来写死 1（TLS），
         *   连自建服务端时改不了；现在由 cfg.json 的 xiaozhi_ssl 决定。
         *   wsock_connect 的 port 同理，原来是 LWIP_IANA_PORT_HTTPS 常量。 */
        wsock_init(&xz_app.ws.clnt, xz_server_ssl() ? 1 : 0, 1, my_wsapp_fn);
        result = wsock_connect(&xz_app.ws.clnt, MAX_WSOCK_HDR_LEN,
                               xz_server_host(), xz_server_path(),
                               xz_server_port(), xz_server_token(), NULL,
                               "Protocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
                               get_mac_address(), client_id);
        LOG_I("Web socket connection attempt %d: %d\n", 10 - retry, result);
        if (result == 0)
        {
            /* Use longer timeout, follow best practices */
            if (rt_sem_take(xz_app.ws.sem, WEBSOCKET_CONNECTION_TIMEOUT_MS) == RT_EOK)
            {
                if (xz_app.ws.is_connected)
                {
                    /* Reconnection successful, clear reconnect flag */
                    xz_app.websocket_reconnect_flag = 0;
                    result = wsock_write(&xz_app.ws.clnt, HELLO_MESSAGE,
                                         strlen(HELLO_MESSAGE), OPCODE_TEXT);
                    LOG_I("Web socket write %d\r\n", result);
                    if (result == ERR_OK)
                    {
                        LOG_I("WebSocket reconnection successful\n");
                        return;
                    }
                    else
                    {
                        LOG_E("Failed to send hello message: %d, retrying...\n", result);
                    }
                }
                else
                {
                    LOG_E("Web socket connection established but not properly initialized, retrying...\n");
                }
            }
            else
            {
                LOG_E("Web socket connection timeout after 50 seconds, retrying...\n");
            }
        }
        else
        {
            LOG_E("Web socket connect failed: %d, retry %d remaining...\n", result, retry);
        }

        /* Optimized retry interval - reduced for faster reconnection */
        uint32_t delay_ms = RETRY_DELAY_BASE_MS + (10 - retry) * RETRY_DELAY_INCREMENT_MS; // 1s-2.8s递增
        LOG_D("Waiting %d ms before next retry...\n", delay_ms);
        rt_thread_mdelay(delay_ms);
    }

    /* 重连失败，清除重连标志 */
    xz_app.websocket_reconnect_flag = 0;
    LOG_E("Web socket reconnect failed after all retries\n");

    /* 复位状态 */
    xz_app.state = kDeviceStateUnknown;
    xiaozhi_ui_chat_status("   连接失败");
    xiaozhi_ui_chat_output("请重试");
}

void xz_ws_audio_init(void)
{
    static uint8_t init_flag = 1;
    if (init_flag)
    {
        xz_audio_decoder_encoder_open(1);
        xz_mic_init();
        xz_button_init();
        xz_event_init();
        xz_sound_init();

        LOG_I("Audio system initialized successfully");

        /* Create TTS sentence end timer once here to avoid race conditions
         * Timer is one-shot and will be started on TTS start/sentence_end events */
        if (!xz_app.tts_sentence_end_timer)
        {
            xz_app.tts_sentence_end_timer = rt_timer_create("tts_end_timer",
                                                           tts_sentence_end_timeout,
                                                           RT_NULL,
                                                           rt_tick_from_millisecond(TTS_SENTENCE_TIMEOUT_MS),
                                                           RT_TIMER_FLAG_ONE_SHOT);
            if (xz_app.tts_sentence_end_timer)
            {
                LOG_D("Created TTS sentence end timer (%d ms)", TTS_SENTENCE_TIMEOUT_MS);
            }
            else
            {
                LOG_E("Failed to create TTS sentence end timer");
            }
        }

        /* Create workqueue for TTS stop delayed restart */
        if (!xz_app.tts_stop_workqueue)
        {
            xz_app.tts_stop_workqueue = rt_workqueue_create("tts_stop_wq", 2048, RT_THREAD_PRIORITY_MAX - 1);
            if (xz_app.tts_stop_workqueue)
            {
                LOG_D("Created TTS stop workqueue");
                /* Initialize the work item */
                rt_work_init(&tts_stop_work, tts_stop_restart_listening, RT_NULL);
            }
            else
            {
                LOG_E("Failed to create TTS stop workqueue");
            }
        }

        /* Create TTS stop delay timer */
        if (!tts_stop_delay_timer)
        {
            tts_stop_delay_timer = rt_timer_create("tts_stop_delay",
                                                   tts_stop_delay_timeout,
                                                   RT_NULL,
                                                   rt_tick_from_millisecond(TTS_STOP_DELAY_MS),
                                                   RT_TIMER_FLAG_ONE_SHOT);
            if (tts_stop_delay_timer)
            {
                LOG_D("Created TTS stop delay timer (%d ms)", TTS_STOP_DELAY_MS);
            }
            else
            {
                LOG_E("Failed to create TTS stop delay timer");
            }
        }

        init_flag = 0;
    }
}

/* IoT device management functions */
void send_iot_states(void)
{
    const char *state = iot_get_states_json();
    if (state == NULL)
    {
        LOG_E("Failed to get IoT states");
        return;
    }

    /* state 可能较长，动态分配缓冲 */
    int state_len = strlen(state);
    int msg_size = state_len + 256; /* 为 session_id 等预留额外空间 */
    char *msg = (char *)rt_malloc(msg_size);
    if (msg == NULL)
    {
        LOG_E("Failed to allocate memory for IoT states");
        return;
    }

    snprintf(msg, msg_size,
             "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,"
             "\"states\":%s}",
             xz_app.ws.session_id, state);
    LOG_D("Sending IoT states:\n%s\n", msg);
    if (xz_app.ws.is_connected)
    {
        wsock_write(&xz_app.ws.clnt, msg, strlen(msg), OPCODE_TEXT);
    }
    else
    {
        LOG_W("websocket is not connected");
    }
    rt_free(msg);
}

void send_iot_descriptors(void)
{
    const char *desc = iot_get_descriptors_json();
    if (desc == NULL)
    {
        LOG_E("Failed to get IoT descriptors");
        return;
    }

    // Dynamically allocate buffer since descriptor may be long
    int desc_len = strlen(desc);
    int msg_size = desc_len + 256; // Extra space for session_id etc.
    char *msg = (char *)rt_malloc(msg_size);
    if (msg == NULL)
    {
        LOG_E("Failed to allocate memory for IoT descriptors");
        return;
    }

    snprintf(msg, msg_size,
             "{\"session_id\":\"%s\",\"type\":\"iot\",\"update\":true,"
             "\"descriptors\":%s}",
             xz_app.ws.session_id, desc);
    LOG_D("Sending IoT descriptors:\n%s", msg);
    if (xz_app.ws.is_connected)
    {
        wsock_write(&xz_app.ws.clnt, msg, strlen(msg), OPCODE_TEXT);
    }
    else
    {
        LOG_W("websocket is not connected");
    }
    rt_free(msg);
}

/* Message processing functions */
char *my_json_string(cJSON *json, char *key)
{
    cJSON *item = cJSON_GetObjectItem(json, key);
    if (item && cJSON_IsString(item))
    {
        return item->valuestring;
    }
    return "";
}

void Message_handle(const uint8_t *data, uint16_t len)
{
    cJSON *root = cJSON_Parse((const char *)data);
    if (!root)
    {
        LOG_E("Error before: [%s]\n", cJSON_GetErrorPtr());
        return;
    }

    char *type = my_json_string(root, "type");

    if (strcmp(type, "hello") == 0)
    {
        char *session_id = cJSON_GetObjectItem(root, "session_id")->valuestring;
        cJSON *audio_param = cJSON_GetObjectItem(root, "audio_params");
        xz_app.ws.sample_rate = cJSON_GetObjectItem(audio_param, "sample_rate")->valueint;
        xz_app.ws.frame_duration = cJSON_GetObjectItem(audio_param, "frame_duration")->valueint;
        strncpy(xz_app.ws.session_id, session_id, 9);
        rt_bool_t in_listening = (xz_app.state == kDeviceStateListening) || xz_mic_is_enabled();
        if (!in_listening && xz_app.state != kDeviceStateSpeaking)
        {
            xz_app.state = kDeviceStateIdle;
        }
        xz_ws_audio_init();

        /* Initialize only on first connection */
        if (!xz_app.iot_initialized)
        {
            LOG_I("Initializing IoT devices for first time\n");
            extern void iot_initialize(void);
            iot_initialize();
            xz_app.iot_initialized = 1;
        }
        else
        {
            LOG_D("IoT already initialized, skipping repeated initialization\n");
        }

        /* Resend device info after each reconnect */
        send_iot_descriptors();
        send_iot_states();
        rt_bool_t pending_listen = xz_app.pending_listen_start;
        if (!pending_listen && !in_listening)
        {
            xiaozhi_ui_chat_status("   待命中");
            xiaozhi_ui_chat_output(" ");
                LOG_I("Waiting...\n");
        }
        else
        {
            LOG_I("Pending wake-up detected on hello, preparing to listen\n");
        }

        if (pending_listen)
        {
            if (xz_app.state == kDeviceStateListening || xz_app.state == kDeviceStateSpeaking)
            {
                xz_app.pending_listen_start = RT_FALSE;
                return;
            }

            xz_app.pending_listen_start = RT_FALSE;


            xz_app.state = kDeviceStateListening;
            xz_mic(1);
            /* 必须为 manual_stop，与按键路径保持一致。
             *
             * 该路径对应"未连接时按下按钮，连上后自动开麦"。
             * 原先使用 auto_stop（由服务端 VAD 判定静音后收尾），但按住
             * 说话的语义是松手即停：环境存在背景音时 VAD 无法判定静音，
             * 松手后麦克风持续录音，用户会认为按键未生效。
             * BUTTON_EVENT_RELEASED 分支发送的 listen stop 在 auto_stop 下
             * 仍然有效，但两处模式不一致会使行为取决于"哪条路径开的麦"，
             * 该差异无法预期也难以调试。 */
            if (ws_send_listen_start(&xz_app.ws.clnt, xz_app.ws.session_id, kListeningModeManualStop))
            {
                xiaozhi_ui_chat_status("   聆听中");
                xiaozhi_ui_chat_output("聆听中...");
            }
            else
            {
                LOG_W("Listen start failed after hello, falling back to idle");
                xz_app.state = kDeviceStateIdle;
                xz_mic(0);
                xiaozhi_ui_chat_status("   就绪");
                xiaozhi_ui_chat_output("就绪");
            }
        }
    }
    else if (strcmp(type, "goodbye") == 0)
    {
        /* Ensure mic is closed when entering sleep */
        xz_mic(0);
        xiaozhi_ui_chat_status("   休眠中");
        xiaozhi_ui_chat_output("等待唤醒");
        xz_app.state = kDeviceStateUnknown;
        LOG_I("session ended\n");

        /* Reset the initialization flag for next session */
    }
    else if (strcmp(type, "tts") == 0)
    {
        char *state = my_json_string(root, "state");
        if (strcmp(state, "start") == 0)
        {
            if (xz_app.state == kDeviceStateIdle || xz_app.state == kDeviceStateListening)
            {
                /* Ensure mic off before TTS starts */
                if (xz_app.state == kDeviceStateListening)
                {
                    xz_mic(0);
                }

                xz_app.state = kDeviceStateSpeaking;
                xiaozhi_ui_chat_status("   说话中");
                xz_speaker(1);
                LOG_D("State transitioned to Speaking, microphone stopped\n");

                /* Start fallback timeout in case sentence_end never arrives */
                if (xz_app.multi_turn_conversation_enabled && xz_app.tts_sentence_end_timer)
                {
                    rt_timer_stop(xz_app.tts_sentence_end_timer);
                    rt_err_t start_ret = rt_timer_start(xz_app.tts_sentence_end_timer);
                    if (start_ret == RT_EOK)
                    {
                        LOG_D("Started TTS sentence end timer on TTS start (%d ms)", TTS_SENTENCE_TIMEOUT_MS);
                    }
                    else
                    {
                        LOG_W("Failed to start TTS sentence end timer on TTS start, start_ret=%d", start_ret);
                    }
                }
            }
            else
            {
                LOG_D("Already in Speaking state, ignoring duplicate start\n");
            }
        }
        else if (strcmp(state, "stop") == 0)
        {
            /* Stop the sentence end timer if it's running */
            if (xz_app.tts_sentence_end_timer)
            {
                rt_timer_stop(xz_app.tts_sentence_end_timer);
                LOG_D("Stopped TTS sentence end timer on TTS stop");
            }

            xz_app.state = kDeviceStateIdle;
            xz_speaker(0);

            /* Ensure microphone is closed when conversation ends */
            if (xz_mic_is_enabled())
            {
                xz_mic(0);
            }

            /* Microphone recording should have been stopped already */
            LOG_D("TTS stopped: mic enabled=%d", xz_mic_is_enabled());

            xiaozhi_ui_chat_status("   就绪");
            xiaozhi_ui_chat_output("就绪");
            LOG_D("TTS stopped, state reset to Idle\n");

            /* Check if multi-turn conversation is enabled */
            if (xz_app.multi_turn_conversation_enabled)
            {
                /* Multi-turn conversation: start delay timer to restart listening after audio finishes */
                if (tts_stop_delay_timer)
                {
                    LOG_D("Starting TTS stop delay timer (%d ms)", TTS_STOP_DELAY_MS);
                    rt_timer_start(tts_stop_delay_timer);
                }
                else
                {
                    LOG_W("Delay timer not available, restarting listening immediately");
                    /* Fallback: restart immediately */
                    xz_app.state = kDeviceStateListening;
                    xz_mic(1);
                    if (ws_send_listen_start(&xz_app.ws.clnt, xz_app.ws.session_id, kListeningModeAutoStop))
                    {
                        xiaozhi_ui_chat_status("   聆听中");
                        xiaozhi_ui_chat_output("聆听中...");
                    }
                    else
                    {
                        LOG_W("Listen start failed in TTS stop handler");
                        xz_app.state = kDeviceStateIdle;
                        xz_mic(0);
                        xiaozhi_ui_chat_status("   就绪");
                        xiaozhi_ui_chat_output("就绪");
                    }
                }
            }
        }
        else if (strcmp(state, "sentence_start") == 0)
        {
            LOG_I("tts:%s", my_json_string(root, "text"));
            xiaozhi_ui_chat_output(my_json_string(root, "text"));
        }
        else if (strcmp(state, "sentence_end") == 0)
        {
            /* sentence_end indicates the end of a sentence */
            LOG_D("TTS sentence ended");

            /* For multi-turn conversation, start a timeout timer to restart listening after sentence end */
            if (xz_app.multi_turn_conversation_enabled && xz_app.state == kDeviceStateSpeaking)
            {
                /* Use the pre-created timer: stop then start to reset timeout */
                if (xz_app.tts_sentence_end_timer)
                {
                    rt_err_t stop_ret = rt_timer_stop(xz_app.tts_sentence_end_timer);
                    if (stop_ret != RT_EOK)
                    {
                        LOG_W("rt_timer_stop returned %d when resetting TTS timer", stop_ret);
                    }
                    rt_err_t start_ret = rt_timer_start(xz_app.tts_sentence_end_timer);
                    if (start_ret == RT_EOK)
                    {
                        LOG_D("Started TTS sentence end timer (%d ms), stop_ret=%d start_ret=%d", TTS_SENTENCE_TIMEOUT_MS, stop_ret, start_ret);
                    }
                    else
                    {
                        LOG_E("Failed to start existing TTS sentence end timer, start_ret=%d", start_ret);
                    }
                }
                else
                {
                    /* Fallback: try to create and start the timer if it wasn't created earlier */
                    xz_app.tts_sentence_end_timer = rt_timer_create("tts_end_timer",
                                                                   tts_sentence_end_timeout,
                                                                   RT_NULL,
                                                                   rt_tick_from_millisecond(TTS_SENTENCE_TIMEOUT_MS),
                                                                   RT_TIMER_FLAG_ONE_SHOT);
                    if (xz_app.tts_sentence_end_timer && rt_timer_start(xz_app.tts_sentence_end_timer) == RT_EOK)
                    {
                        LOG_D("Created and started fallback TTS sentence end timer (%d ms)", TTS_SENTENCE_TIMEOUT_MS);
                    }
                    else
                    {
                        LOG_E("Failed to create/start fallback TTS sentence end timer");
                    }
                }
            }
        }
        else
        {
            LOG_E("Unknown tts state: %s\n", state);
        }
    }
    else if (strcmp(type, "llm") == 0)
    {
        LOG_I("llm emotion: %s", cJSON_GetObjectItem(root, "emotion")->valuestring);
    }
    else if (strcmp(type, "stt") == 0)
    {
        LOG_I("stt:%s", cJSON_GetObjectItem(root, "text")->valuestring);
        xiaozhi_port_on_stt(cJSON_GetObjectItem(root, "text")->valuestring);  // 本工程：识别文本上屏（适配层，见 xiaozhi_port.c）
    }
    else if (strcmp(type, "iot") == 0)
    {
        LOG_D("iot command");
        cJSON *commands = cJSON_GetObjectItem(root, "commands");
        for (int i = 0; i < cJSON_GetArraySize(commands); i++)
        {
            cJSON *cmd = cJSON_GetArrayItem(commands, i);
            char *cmd_str = cJSON_PrintUnformatted(cmd);
            if (cmd_str)
            {
                iot_invoke((uint8_t *)cmd_str, strlen(cmd_str));
                send_iot_states();
                cJSON_free(cmd_str);
            }
        }
    }
    else if (strcmp(type, "mcp") == 0)
    {
        LOG_D("mcp command");
        cJSON *payload = cJSON_GetObjectItem(root, "payload");
        if (payload && cJSON_IsObject(payload))
        {
            // extern void McpServer_ParseMessage(const char *message);
            char *payload_str = cJSON_PrintUnformatted(payload);
            if (payload_str)
            {
                McpServer_ParseMessage(payload_str);
                cJSON_free(payload_str);
            }
        }
    }
    else if (strcmp(type, "error") == 0)
    {
        cJSON *message = cJSON_GetObjectItem(root, "message");
        if (message && cJSON_IsString(message))
        {
            LOG_E("Server error: %s\n", message->valuestring);
        }
        else
        {
            LOG_E("Server returned error\n");
        }
    }
    else
    {
        LOG_E("Unknown type: %s\n", type);
    }
    cJSON_Delete(root);
}

/* Network utility functions */
void svr_found_callback(const char *name, const ip_addr_t *ipaddr, void *callback_arg)
{
    if (ipaddr != NULL)
    {
        LOG_D("DNS lookup succeeded, IP: %s\n", ipaddr_ntoa(ipaddr));
    }
}

int check_internet_access(void)
{
    const char *hostname = xz_server_host();
    ip_addr_t addr = {0};
    err_t err;

    /* 自建服务端通常直接填写 IP（如 192.168.1.100），此时不应走 DNS：
     * dns_gethostbyname 对 IP 字面量的行为依赖 lwIP 版本，实测在启用
     * LWIP_DNS 且未配置 DNS 服务器时返回超时，会将"服务端实际可达"
     * 误判为"没有网络"，导致设备始终停留在"等待网络"。
     * 先尝试 inet_aton 直接解析，成功即说明网络栈存在且地址合法。 */
    if (ip4addr_aton(hostname, ip_2_ip4(&addr)) != 0)
        return 1;

    err = dns_gethostbyname(hostname, &addr, svr_found_callback, NULL);
    return (err == ERR_OK || err == ERR_INPROGRESS) ? 1 : 0;
}

char *get_xiaozhi_ws(void)
{
    char *buffer = RT_NULL;
    int resp_status;
    struct webclient_session *session = RT_NULL;
    char *xiaozhi_url = RT_NULL;
    int content_length = -1, bytes_read = 0, content_pos = 0;

    if (!check_internet_access())
    {
        return buffer;
    }

    int size = strlen(OTA_VERSION) + MAX_CLIENT_ID_LEN +
               MAX_MAC_ADDR_LEN * 2 + 16;
    char *ota_formatted = (char *)rt_malloc(size);
    if (!ota_formatted)
    {
        goto __exit;
    }

    rt_snprintf(ota_formatted, size, OTA_VERSION, get_mac_address(),
                get_client_id(), get_mac_address());
    xiaozhi_url = (char *)rt_calloc(1, GET_URL_LEN_MAX);
    if (!xiaozhi_url)
    {
        LOG_E("No memory for xiaozhi_url!\n");
        goto __exit;
    }

    /* OTA 地址同样运行时拼：https://host:port/xiaozhi/ota/ 或 http://...。
     * 原来固定用 GET_URI 宏 + XIAOZHI_HOST，指向自建服务端时改不了。 */
    xz_server_ota_url(xiaozhi_url, GET_URL_LEN_MAX);
    LOG_I("OTA url: %s", xiaozhi_url);

    session = webclient_session_create(1024);
    if (!session)
    {
        LOG_E("No memory for get header!\n");
        goto __exit;
    }

    webclient_header_fields_add(session, "Device-Id: %s \r\n", get_mac_address());
    webclient_header_fields_add(session, "Client-Id: %s \r\n", get_client_id());
    webclient_header_fields_add(session, "Content-Type: application/json \r\n");
    webclient_header_fields_add(session, "Content-length: %d \r\n",
                                strlen(ota_formatted));

    if ((resp_status = webclient_post(session, xiaozhi_url, ota_formatted,
                                      strlen(ota_formatted))) != 200)
    {
        LOG_E("webclient Post request failed, response(%d) error.\n", resp_status);
    }

    buffer = (char *)rt_calloc(1, GET_RESP_BUFSZ);
    if (!buffer)
    {
        LOG_E("No memory for data receive buffer!\n");
        goto __exit;
    }

    content_length = webclient_content_length_get(session);
    if (content_length > 0)
    {
        do
        {
            bytes_read = webclient_read(session, buffer + content_pos,
                                        content_length - content_pos > GET_RESP_BUFSZ
                                            ? GET_RESP_BUFSZ
                                            : content_length - content_pos);
            if (bytes_read <= 0)
            {
                break;
            }
            content_pos += bytes_read;
        } while (content_pos < content_length);
    }
    else
    {
        rt_free(buffer);
        buffer = NULL;
    }

__exit:
    if (xiaozhi_url)
        rt_free(xiaozhi_url);
    if (session)
        webclient_close(session);
    if (ota_formatted)
        rt_free(ota_formatted);
    return buffer;
}

int http_xiaozhi_data_parse_ws(char *json_data)
{
    cJSON *root = cJSON_Parse(json_data);
    if (!root)
    {
        LOG_E("Error before: [%s]\n", cJSON_GetErrorPtr());
        return -1;
    }

    xiaozhi_ws_connect();
    cJSON_Delete(root);
    return 0;
}

void xiaozhi_ws_connect(void)
{
    err_t err;
    uint32_t retry = 10;

    while (retry-- > 0)
    {
        /* Check network connection status */
        if (!check_internet_access())
        {
            LOG_I("Waiting internet ready... (%d retries remaining)\n", retry);
            xiaozhi_ui_chat_status("   等待网络");
            xiaozhi_ui_chat_output("检查网络连接...");
            rt_thread_mdelay(500); /* Reduced network check delay */
            continue;
        }

        /* Ensure WebSocket in correct state */
        if (xz_app.ws.clnt.pcb != RT_NULL && ((struct tcp_pcb *)xz_app.ws.clnt.pcb)->state != CLOSED)
        {
            LOG_D("Cleaning up existing WebSocket connection\n");
            wsock_close(&xz_app.ws.clnt, WSOCK_RESULT_LOCAL_ABORT, ERR_OK);
            rt_thread_mdelay(200); /* Reduced cleanup delay */
        }

        if (!xz_app.ws.sem)
        {
            xz_app.ws.sem = rt_sem_create("xz_ws", 0, RT_IPC_FLAG_FIFO);
        }

        // Create WebSocket write mutex
        if (!xz_app.ws.ws_write_mutex)
        {
            xz_app.ws.ws_write_mutex = rt_mutex_create("xz_ws_write", RT_IPC_FLAG_FIFO);
            if (!xz_app.ws.ws_write_mutex)
            {
                LOG_E("Failed to create WebSocket write mutex\n");
                continue;
            }
        }

        /* 与 reconnect_websocket 里的连接点保持一致：参数全部运行时取，
         * 详见 xz_server_cfg.h。 */
        wsock_init(&xz_app.ws.clnt, xz_server_ssl() ? 1 : 0, 1, my_wsapp_fn);
        char *client_id = get_client_id();
        err = wsock_connect(&xz_app.ws.clnt, MAX_WSOCK_HDR_LEN,
                            xz_server_host(), xz_server_path(),
                            xz_server_port(), xz_server_token(), NULL,
                            "Protocol-Version: 1\r\nDevice-Id: %s\r\nClient-Id: %s\r\n",
                            get_mac_address(), client_id);
        LOG_I("Web socket connection attempt %d: %d\n", 10 - retry, err);

        if (err == 0)
        {
            /* Connection successful, wait for handshake */
            if (rt_sem_take(xz_app.ws.sem, WEBSOCKET_CONNECTION_TIMEOUT_MS) == RT_EOK)
            {
                LOG_I("WebSocket handshake completed, connected=%d\n", xz_app.ws.is_connected);
                if (xz_app.ws.is_connected)
                {
                    err = wsock_write(&xz_app.ws.clnt, HELLO_MESSAGE,
                                      strlen(HELLO_MESSAGE), OPCODE_TEXT);
                    if (err == ERR_OK)
                    {
                        LOG_I("Initial WebSocket connection established successfully\n");
                        return;
                    }
                    else
                    {
                        LOG_E("Failed to send hello message: %d\n", err);
                    }
                }
                else
                {
                    LOG_E("WebSocket connected but not properly initialized\n");
                }
            }
            else
            {
                LOG_E("WebSocket connection timeout after 50 seconds\n");
            }
        }
        else
        {
            LOG_E("WebSocket connection failed: %d, %d retries remaining\n", err, retry);
        }

        /* Connection failed, update UI status */
        if (retry > 0)
        {
            xiaozhi_ui_chat_status("   连接失败");
            char retry_msg[64];
            rt_snprintf(retry_msg, sizeof(retry_msg), "Retrying... (%d)", 10 - retry);
            xiaozhi_ui_chat_output(retry_msg);
            rt_thread_mdelay(1000); /* Reduced retry delay for better responsiveness */
        }
    }

    /* All retries failed */
    LOG_E("WebSocket connection failed after all attempts\n");
    xiaozhi_ui_chat_status("   连接失败");
    xiaozhi_ui_chat_output("请检查网络并重试");
}

/* Application entry point */
void xiaozhi_entry(void *p)
{
    char *my_ota_version;

    while (!xz_app.exit_req)
    {
        my_ota_version = get_xiaozhi_ws();
        if (my_ota_version)
        {
            http_xiaozhi_data_parse_ws(my_ota_version);
            rt_free(my_ota_version);
            break;
        }
        else
        {
            LOG_E("Waiting internet... \n");
            rt_thread_mdelay(1000);
        }
    }

    if (xz_app.exit_req)
        LOG_I("xiaozhi thread exit by request");

    /* 线程自行清除句柄，使 xz_app_stop 的等待能够结束。ws 与音频的关闭
     * 交给 xz_app_stop 在 join 之后执行，因为本线程退出时仍可能在使用它们。 */
    xz_app.xiaozhi_tid = RT_NULL;
}

/* ---------- 给 C 侧（xz_app.c）的访问函数 ----------
 * xz_app.c 是 C 编译单元，不能包含 xiaozhi.h（C++ 聚合头，依赖 lwip
 * websocket、opus、cJSON）。因此所有内部状态读取都集中到这几个函数，
 * 避免在 C 侧推测结构体布局。 */
/* "小智是否在运行"。
 *
 * 不能仅依据 xiaozhi_tid != RT_NULL：xiaozhi_entry 在连接成功后执行 break
 * 退出循环，并将 tid 置为 NULL（见 xiaozhi_entry 结尾）。即连接服务端成功的
 * 那一刻 tid 即变为 NULL；若以 tid 判断"在运行"，状态字会始终停留在
 * STOPPED、应用按钮始终为 START、app_registry 的 is_running 恒为假。
 *
 * 实际语义应为"该语音链路仍然存活"，三个证据任一成立即可：
 *   1. tid 非空：仍在连接或重连
 *   2. ws 已连接：正常工作的主状态（线程早已退出，但链路仍在用）
 *   3. state 不是未知：状态机已运行过，说明链路曾经启动
 * 第 3 条用于连接断开后重连的窗口期：此时 tid 与 is_connected 都可能为假，
 * 但用户在视角上"小智仍然开着"，界面不应退回 STOPPED 要求用户重新点 START。
 *
 * 退出状态由 exit_req 显式排除：xz_app_stop 置位后即使 ws 尚未断开，也须
 * 立即表现为停止，否则界面会在按下 STOP 后仍显示 RUNNING。 */
extern "C" int xz_state_is_running(void)
{
    if (xz_app.exit_req)
        return 0;

    if (xz_app.xiaozhi_tid != RT_NULL)
        return 1;

    if (xz_app.ws.is_connected)
        return 1;

    if (xz_app.state != kDeviceStateUnknown)
        return 1;

    return 0;
}

extern "C" int xz_state_is_connected(void)
{
    return (xz_app.ws.is_connected != 0) ? 1 : 0;
}

/* 状态机线程是否已经结束。
 *
 * xiaozhi_entry 在退出前会自行将 xiaozhi_tid 置 NULL（见该函数结尾），
 * 这比 rt_thread_find("xiaozhi_thread") 更可靠：_thread_exit() 仅将线程
 * 挂入 defunct 队列，对象需等空闲线程回收后才真正删除，在此之前
 * rt_thread_find 仍能命中，会使 xz_app_stop 空等 5 秒。 */
extern "C" int xz_state_thread_gone(void)
{
    return (xz_app.xiaozhi_tid == RT_NULL) ? 1 : 0;
}

extern "C" int xz_state_device_state(void)
{
    return (int)xz_app.state;
}

extern "C" void xz_app_exit_req(rt_bool_t on)
{
    xz_app.exit_req = on;
}

/* 停止后复位链路状态。
 *
 * 需要该函数的原因：xz_state_is_running() 现将对"ws 已连接"与"state 非
 * Unknown"也视为在运行（因为连接成功后线程即退出，tid 已为 NULL）。
 * 因此 xz_app_stop 置位 exit_req 可立即表现为停止，但 exit_req 一旦复位，
 * 旧连接与旧状态又会使 running 恢复为真，下次点击 START 会被 "already
 * running" 拦截，导致无法再次启动。
 *
 * 此处真正关闭 ws 并将相关标志归零，使 stop 之后处于干净的 Unknown 状态。 */
extern "C" void xz_app_reset_link_state(void)
{
    /* 仅在 wsock 确实初始化过之后才调用 wsock_close。
     *
     * wsock_close 开头有 LWIP_ASSERT("pws->state0 == alloc'd")，对从未
     * init 过的状态结构调用会直接触发断言并复位。因此先验证 state0 与
     * state1 均为 INITD（取值见 websocket_client.h 的 PWS_STATE_INITD）。 */
    if (xz_app.ws.clnt.pcb != RT_NULL &&
        xz_app.ws.clnt.state0 == PWS_STATE_INITD &&
        xz_app.ws.clnt.state1 == PWS_STATE_INITD)
    {
        wsock_close(&xz_app.ws.clnt, WSOCK_RESULT_LOCAL_ABORT, ERR_OK);
    }

    xz_app.ws.is_connected = 0;
    xz_app.ws.session_id[0] = '\0';
    xz_app.websocket_reconnect_flag = 0;
    xz_app.state = kDeviceStateUnknown;
}

int ws_xiaozhi_init(void)
{
    if (xz_app.xiaozhi_tid != RT_NULL)
    {
        LOG_W("[%s] already running", __FUNCTION__);
        return RT_EOK;
    }

    xz_app.exit_req = RT_FALSE;
    xz_app.xiaozhi_tid = rt_thread_create("xiaozhi_thread", xiaozhi_entry,
                                         (void *)0x01, 1024 * 20, 15, 5);
    if (!xz_app.xiaozhi_tid)
    {
        LOG_E("[%s] Create failed!\n", __FUNCTION__);
        return -RT_ENOMEM;
    }

    if (rt_thread_startup(xz_app.xiaozhi_tid) != RT_EOK)
    {
        LOG_E("[%s] Startup failed!\n", __FUNCTION__);
        return -RT_ERROR;
    }

    LOG_I("[%s] Created successfully\n", __FUNCTION__);
    return RT_EOK;
}

/* Multi-turn conversation control functions */
void xz_enable_multi_turn_conversation(rt_bool_t enable)
{
    xz_app.multi_turn_conversation_enabled = enable;
    LOG_I("Multi-turn conversation %s", enable ? "enabled" : "disabled");
}

rt_bool_t xz_is_multi_turn_conversation_enabled(void)
{
    return xz_app.multi_turn_conversation_enabled;
}

/* Audio notification functions */
void xz_play_power_on_sound(void)
{
    LOG_I("Playing power-on sound");
    if (wavplayer_play("/webnet/power_on.wav") != 0)
    {
        LOG_W("Failed to play power-on sound");
    }
}

