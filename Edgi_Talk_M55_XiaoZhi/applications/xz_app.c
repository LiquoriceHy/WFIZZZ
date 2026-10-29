/*
 * xz_app.c - 小智语音对话的应用层包装
 *
 * 线程模型：
 *   xiaozhi_thread  状态机（xiaozhi.cpp），连上后自行派生以下线程：
 *     event_thread  按键事件
 *     mic_thread    采集
 *     opus          编解码与播放
 * 这些线程全部由小智内部按需创建，本层只负责 start/stop 与状态转换。
 *
 * 停止顺序（重要）：
 *   1. 置 xz_app.exit_req，xiaozhi_entry 的重连循环检查该标志
 *   2. 等待 xiaozhi_thread 退出（该线程自行清除 xiaozhi_tid）
 *   3. 关闭 ws、停止 opus 线程与 ringbuffer、关闭 mic 与 sound
 * 顺序颠倒会在 ws 仍被使用时将其关闭，导致野指针。
 */
#include <rtthread.h>
#include <string.h>

#include "xz_app.h"
#include "mic_arb.h"
#include "xiaozhi_port.h"

/* xiaozhi.cpp 与 xiaozhi_audio.cpp 的符号。不包含 xiaozhi.h，因为它是
 * C++ 头的聚合体（lwip websocket、opus、cJSON），C 编译单元无法包含。
 * 内部状态通过以下 C 访问函数取值，不推测结构体布局。 */
extern int  xz_state_is_connected(void);
extern int  xz_state_device_state(void);
extern int  xz_state_is_running(void);
extern int  xz_state_thread_gone(void);
extern int  ws_xiaozhi_init(void);
extern void xz_app_exit_req(rt_bool_t on);
extern void xz_app_reset_link_state(void);
extern void xz_audio_decoder_encoder_close(void);
extern int  xz_mic_is_enabled(void);

/* 触摸"按住说话"按钮的入口。实现位于 xiaozhi.cpp，因为只有那里能直接
 * 投递到 xz_app.button_event 事件组，与实体按键走同一条事件通道。
 * 返回 0 表示事件已投递，负值表示当前不可用（未初始化或未连接）。 */
extern int  xz_touch_ptt(rt_bool_t pressed);

/* kDeviceState* 枚举在 xiaozhi.h:87-99，按声明顺序取 kDeviceStateSpeaking */
#define XZ_STATE_SPEAKING   6

#define DBG_TAG "xz.app"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* xiaozhi_audio.cpp 内部定义，无独立头文件 */
extern void xz_audio_decoder_encoder_close(void);
extern int  xz_mic_is_enabled(void);

void xz_app_get_status(xz_app_status_t *st)
{
    if (st == RT_NULL)
        return;

    rt_memset(st, 0, sizeof(*st));

    st->running      = (xz_state_is_running() != 0);
    st->ws_connected = (xz_state_is_connected() != 0);
    st->listening    = (xz_mic_is_enabled() != 0);
    st->state        = (rt_uint8_t)xz_state_device_state();

    /* 文案必须是 ASCII：CJK 字体在本工程未启用时中文整段不绘制。
     * 状态优先级：未启动 > 未连 ws > 正在说话 > 正在听 > 空闲 */
    if (!st->running)
        rt_snprintf(st->status, sizeof(st->status), "stopped");
    else if (!st->ws_connected)
        rt_snprintf(st->status, sizeof(st->status), "connecting...");
    else if (xz_state_device_state() == XZ_STATE_SPEAKING)
        rt_snprintf(st->status, sizeof(st->status), "speaking");
    else if (st->listening)
        rt_snprintf(st->status, sizeof(st->status), "listening");
    else
        rt_snprintf(st->status, sizeof(st->status), "idle");
}

rt_bool_t xz_app_is_running(void)
{
    return (xz_state_is_running() != 0) ? RT_TRUE : RT_FALSE;
}

int xz_app_touch_press(void)
{
    return xz_touch_ptt(RT_TRUE);
}

int xz_app_touch_release(void)
{
    return xz_touch_ptt(RT_FALSE);
}

int xz_app_start(void)
{
    extern int ws_xiaozhi_init(void);
    rt_tick_t t0;
    int i;

    if (xz_state_is_running())
    {
        LOG_I("already running");
        return 0;
    }

    /* 小智整轮对话都要用 mic0（listen 期间开、说/待机时关，但服务端随时
     * 可能要求重新 listen）。这里一次性把 mic0 的独占权拿到手，避免运行
     * 中途被别的模块抢走。仲裁器会先停掉鼾声检测。 */
    if (mic_arb_acquire(MIC_ARB_XIAOZHI) != RT_EOK)
    {
        LOG_E("mic0 unavailable");
        return -RT_EBUSY;
    }

    xz_port_audio_unmute();

    if (ws_xiaozhi_init() != RT_EOK)
    {
        mic_arb_release(MIC_ARB_XIAOZHI);
        return -RT_ERROR;
    }

    /* ws_xiaozhi_init 只创建线程就返回，等它把 tid 之外的初始化跑起来。
     * 不做严格等待：连不上服务端时线程会一直重试，不能阻塞 UI。 */
    t0 = rt_tick_get();
    for (i = 0; i < 50; i++)
    {
        if (!xz_state_is_running() &&
            (rt_tick_get() - t0) > rt_tick_from_millisecond(200))
            break;                    /* 线程已异常退出 */
        rt_thread_mdelay(10);
    }

    LOG_I("started (mic held by xiaozhi)");
    return 0;
}

void xz_app_stop(void)
{
    int waited = 0;

    if (!xz_state_is_running())
    {
        mic_arb_release(MIC_ARB_XIAOZHI);
        return;
    }

    LOG_I("stopping...");

    /* 顺序很重要，分三步：
     *
     * 1. 置 exit_req，使 xiaozhi_entry 的取 OTA 与重连循环尽快收尾。
     * 2. 等待线程真正退出。这一步不能依赖 xz_state_is_running()，
     *    因为该函数现将 "ws 已连接" 也算作在运行（连接成功后线程即退出，
     *    tid 为 NULL），且 exit_req 一旦置起它就立即返回 0。
     *    此处直接检查线程对象是否存在，才是"线程仍存活"的准确信号。
     * 3. 确认线程退出后再关闭 ws 并复位状态。若顺序相反，
     *    xiaozhi_entry 可能正在使用 clnt 发送 hello 或 OTA 请求，
     *    wsock_close 释放 pcb 后将造成 use-after-free。 */
    xz_app_exit_req(RT_TRUE);

    /* 等待状态机线程退出。
     *
     * 使用 rt_thread_find("xiaozhi_thread") 而非 xz_state_is_running()：
     * 后者将 "ws 已连接" 也算作在运行，且 exit_req 一旦置起即返回 0，
     * 无法表达"线程本体是否仍存活"。
     *
     * 注意 _thread_exit() 仅将线程挂入 defunct 队列（RT_THREAD_CLOSE），
     * 真正的 rt_free 由空闲线程回收，回收前 rt_thread_find 仍能命中。
     * 但 xiaozhi_entry 退出前会自行将 xz_app.xiaozhi_tid 置 NULL，
     * 那才是可靠信号；此处两个条件均以 OR 判断，更为稳妥。
     * 等待上限为 5 秒，超时后放弃等待，避免界面停留在 CLOSING。 */
    while (waited < 5000)
    {
        if (rt_thread_find("xiaozhi_thread") == RT_NULL)
            break;
        /* 线程对象仍在 defunct 队列中但已执行完毕，同样视为退出 */
        if (xz_state_thread_gone())
            break;
        rt_thread_mdelay(20);
        waited += 20;
    }

    if (rt_thread_find("xiaozhi_thread") != RT_NULL && !xz_state_thread_gone())
        LOG_W("thread did not exit in 5s (left running)");

    /* 线程已退出（或等待超时），此时关闭 ws 才是安全的 */
    xz_app_reset_link_state();

    /* 收尾音频：opus 线程与 ringbuffer 位于 HyperRAM，不占主堆，
     * 但必须归还，否则下次启动会因 inited 仍为 1 而跳过重建。 */
    xz_audio_decoder_encoder_close();

    xz_app_exit_req(RT_FALSE);
    mic_arb_release(MIC_ARB_XIAOZHI);

    LOG_I("stopped");
}
