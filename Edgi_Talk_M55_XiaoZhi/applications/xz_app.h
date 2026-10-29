/*
 * xz_app.h - 小智语音对话的应用层包装
 *
 * 与 snore_rt / audio_event 同构：后台线程 + volatile 状态 + get_status
 * 快照 + 幂等 start / 异步 stop。UI 只读状态，不碰小智内部。
 *
 * 为什么要这层：
 *   小智原本的入口是一个 init-once 的 ws_xiaozhi_init()，没有停止路径、
 *   没有状态查询。而 app_registry 的契约要求 is_running / start / stop /
 *   get_status 四件套，且启停必须对称。这一层负责：
 *     1. 启动前确保 mic0 可用（经 mic_arb 抢占，必要时停掉鼾声检测）
 *     2. 停止时置 exit_req 并等各线程真正退出，再释放音频资源
 *     3. 把内部的 DeviceState / ws.is_connected 翻译成一行给 UI 看的文案
 */
#ifndef XZ_APP_H
#define XZ_APP_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    rt_bool_t running;          /* xiaozhi_thread 存活 */
    rt_bool_t ws_connected;     /* WebSocket 已连上服务端 */
    rt_bool_t listening;        /* 正在采集（mic0 已开） */
    rt_uint8_t state;           /* 内部 DeviceState 枚举值，调试用 */
    char status[48];            /* 一行状态文案，ASCII */
} xz_app_status_t;

void xz_app_get_status(xz_app_status_t *st);

/* 启动。幂等（已在运行时返回 0）。
 * 内部会先抢占 mic0：若鼾声检测正在跑，会先被停掉并等其退出。
 * 失败返回负 errno。 */
int xz_app_start(void);

/* 请求停止。异步：置标志后各线程自行退出，可轮询 running 确认。
 * 停止过程中会释放 mic0。 */
void xz_app_stop(void);

/* 是否正在运行（供 app_registry 的 is_running 用）。 */
rt_bool_t xz_app_is_running(void);

/* 触摸屏上的"按住说话"按钮。
 *
 * 两者把事件投递到与实体按键相同的事件组，走完全一致的状态机
 * （含未连接时自动重连、播报中打断、松手 manual_stop 收尾）。
 *
 * 返回 0 表示事件已投递；-1 表示小智尚未启动（事件组还没建），
 * UI 应提示用户先点 START。 */
int xz_app_touch_press(void);
int xz_app_touch_release(void);

#ifdef __cplusplus
}
#endif

#endif /* XZ_APP_H */
