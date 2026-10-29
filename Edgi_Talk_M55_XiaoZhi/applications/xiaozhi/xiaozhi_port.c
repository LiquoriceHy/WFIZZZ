/*
 * xiaozhi_port.c - 小智客户端适配层（官方代码零结构改动接入本工程）
 *
 * 职责：
 *  1) 重定向 xiaozhi_ui.h 的 UI 接口：chat_status 转至顶部状态行；
 *     chat_output 转至聊天气泡（role=小智）。但 xiaozhi.cpp 中那批固定
 *     界面文案（"就绪"/"聆听中..."/"正在连接..."等）属于瞬态提示而非对话
 *     内容，按白名单走状态行，不进入气泡；电池、ADC 等接口按需求置为空桩。
 *     数据经 app_data 采用"线程写 volatile，lv_timer 拉取"模式，
 *     不跨线程访问 LVGL。
 *  2) 依赖桩：set_brightness/get_brightness（iot/screen.cpp 引用 pwm/，未恢复）。
 *  3) 静音：按需求不做语音播报，INIT_APP 阶段将 es8388 音量置 0，
 *     TTS 解码与播放管线照常运行（协议状态机不受影响），仅无声音；
 *     MCP 音量工具（self.audio_speaker.set_volume）可随时恢复出声。
 *
 * 唤醒词与 3D 表情已按需求完全移除（2026-10-27）：官方实现从未接入本工程
 * （Edge Impulse 模型未恢复），此前仅为一组恒定返回 FALSE 的桩，保留会使
 * xiaozhi.cpp 中遍布无意义的 is_enabled() 判断。当前为纯按键对话加纯文字
 * 聊天的 chatbox。
 *
 * 注：官方副本内的 g_app 已统一改名为 xz_app（xiaozhi.cpp 的
 * xiaozhi_app_t），避免与本工程 app_data 的 g_app 产生链接冲突。
 */
#include <rtthread.h>
#include <string.h>

#include "app_data.h"
#include "xiaozhi_port.h"
#include "ui/xiaozhi_ui.h"

/* ---------- es8388 音量 ----------
 * 此处原为 INIT_APP_EXPORT(xz_port_mute)，开机时将 ES8388 置 0。
 * 初期小智仅做文字交互、无需出声，该做法没有问题；接入语音对话后，
 * TTS 需从 sound0 播放，开机静音会将其一并屏蔽，并连带静音 wavplayer
 * 等其他音频。现改为由 xz_app_start 在启动时设置正常音量。 */
#if defined(BSP_USING_AUDIO_PLAY)
#define XZ_PORT_VOLUME  60

void xz_port_audio_unmute(void)
{
    extern void es8388_volume_set(rt_uint8_t volume);

    es8388_volume_set(XZ_PORT_VOLUME);
    rt_kprintf("[xz_port] speaker volume %d\n", XZ_PORT_VOLUME);
}
#else
void xz_port_audio_unmute(void) { }
#endif

/* ---------- UI 接口重定向（xiaozhi_ui.h 全集） ---------- */

void xiaozhi_ui_init(void)
{
    /* UI 由 LVGL 线程的 ui_chat 承担，无需独立 UI 线程 */
}

rt_err_t xiaozhi_ui_wait_ready(rt_int32_t timeout)
{
    RT_UNUSED(timeout);
    return RT_EOK;   /* 无独立线程，即到即用 */
}

void xiaozhi_ui_set_status(const char *status)
{
    app_data_set_xiaozhi_status(status);
}

/* xiaozhi.cpp 的固定界面文案：这些属于瞬态提示，不是对话内容。
 *
 * 2026-10-26 修正：旧白名单为 ASCII 版（"Listening..." / "Ready" 等），
 * 而 xiaozhi.cpp 实际发送的是中文（"就绪"/"聆听中"/"待命中"等共 40 余处）。
 * 两者不匹配，导致每条瞬态提示都落入 else 分支，被当作小智的聊天正文
 * 放入气泡，聊天记录中充满"聆听中"/"就绪"，真正的回复被淹没。
 *
 * 当前处理：命中即整体丢弃，既不进入气泡也不写入状态行。不写状态行的
 * 原因是本期未启用中文字库（LV_FONT_SIMSUN_16_CJK 仅含 1118 个稀疏汉字，
 * 且状态行使用纯 ASCII 的 montserrat_16），中文串写入后同样是空白。
 * 聊天页的状态行改由 xz_app_get_status() 提供 ASCII 文案（见 app_registry.c）。
 *
 * 真正的 AI 回复不经过此处：它走 tts 的 sentence_start 到 text 字段，
 * 内容不在下表内，照常进入气泡。 */
static const char *const xz_transient_outputs[] =
{
    /* xiaozhi_ui_chat_output() 的固定文案（xiaozhi.cpp 实测全集） */
    "就绪",
    "聆听中...",
    "等待唤醒",
    "正在连接...",
    "正在连接小智...",
    "重新连接中...",
    "仍在连接中...",
    "我在，请说...",
    "检查网络连接...",
    "请稍后再试",
    "请重试",
    "请检查网络并重试",
    " ",

    /* xiaozhi_ui_chat_status() 的文案（旧代码将它们也经 output 转发，
     * 一并拦截；带前导空格的写法照抄 xiaozhi.cpp 原文） */
    "   连接中",
    "   连接失败",
    "   就绪",
    "   聆听中",
    "   说话中",
    "   待命中",
    "   休眠中",
    "   唤醒",
    "   等待网络",

    /* 兼容性保留：若将来日志/文案改回英文，这些仍然挡得住 */
    "Listening...",
    "Connecting...",
    "Connecting to XiaoZhi...",
    "Still connecting...",
    "Reconnecting...",
    "Checking network...",
    "Network error, retry",
    "Please retry later",
    "Please retry",
    "Say something...",
    "Ready",
    "Waiting for wake word",
};

void xiaozhi_ui_set_output(const char *output)
{
    int i;

    if (output == RT_NULL || output[0] == '\0')
        return;

    for (i = 0; i < (int)(sizeof(xz_transient_outputs) /
                          sizeof(xz_transient_outputs[0])); i++)
    {
        if (rt_strcmp(output, xz_transient_outputs[i]) == 0)
            return;   /* 瞬态提示：丢弃（见函数上方说明） */
    }

    /* 其余进聊天流：tts sentence_start 的正文就是走这条 */
    app_data_set_xiaozhi_msg(1, output);
}

void xiaozhi_port_on_stt(const char *text)
{
    app_data_set_xiaozhi_msg(0, text);   /* 用户消息：识别结果入聊天流 */
}

/* 表情/调试/电池/配网提示：按需求空桩（状态行已覆盖连接提示） */
void xiaozhi_ui_set_adc(const char *adc_str)          { RT_UNUSED(adc_str); }
void xiaozhi_ui_clear_info(void)                      { }
void xiaozhi_ui_update_battery(int level)             { RT_UNUSED(level); }
void xiaozhi_ui_update_charging_status(bool charging) { RT_UNUSED(charging); }
void xiaozhi_ui_update_ble_status(bool connected)     { RT_UNUSED(connected); }
void xiaozhi_ui_show_ap_config(const char *ap_info)   { RT_UNUSED(ap_info); }
void xiaozhi_ui_show_connecting(void)
{
    app_data_set_xiaozhi_status("连接WiFi中");
}

/* 旧名兼容转发（xiaozhi.cpp 实际调用的就是这批） */
void init_ui(void)                                  { xiaozhi_ui_init(); }
rt_err_t wait_ui_ready(rt_int32_t timeout)          { return xiaozhi_ui_wait_ready(timeout); }
void clean_info(void)                               { xiaozhi_ui_clear_info(); }
void xiaozhi_ui_chat_status(char *string)           { xiaozhi_ui_set_status(string); }
void xiaozhi_ui_chat_output(char *string)           { xiaozhi_ui_set_output(string); }
void xiaozhi_ui_update_adc(char *string)            { xiaozhi_ui_set_adc(string); }

/* ---------- iot/ 引用的外部依赖桩 ---------- */

/* 背光亮度（iot/screen.cpp 引用 pwm/pwm.c，未恢复）：空实现 */
void set_brightness(rt_uint8_t percent)
{
    RT_UNUSED(percent);
}

rt_uint8_t get_brightness(void)
{
    return 80;
}
