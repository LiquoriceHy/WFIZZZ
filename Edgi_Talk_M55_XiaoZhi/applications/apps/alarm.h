/*
 * alarm.h - 闹钟管理
 *
 * 闹钟主表在 g_app.alarms（ui_alarm 与 AI 工具经 setter 修改，本模块线程每秒
 * 比对触发，重复模式/单次自动关的规则在此实现）。仅屏幕提醒：触发即置
 * g_app.alarm_ring，由 ui_alarm 的全屏遮罩闪烁；点屏调 app_alarm_stop()，
 * 无人理睬超时自动停。模块自建低优先级线程（仿 maimemo.c），INIT_APP_EXPORT
 * 自启，无需手动调用。
 *
 * 持久化：3 个槽位与便签一起存放在 /sdcard/wfizzz/data.json（见 data_store.h）。
 * 任何改动都只调用 app_alarm_save_request() 置脏，由 app_data 的 1 秒线程
 * 落盘。不在 setter 中同步写卡，因为 AI 工具回调运行于 websocket 线程，
 * 同步写 SD 会阻塞音频上行。
 *
 * 时间源：g_app.time_ts（app_data 每秒从 RTC 读取）；时间无效时不触发。
 * 当前未启用 NTP（PKG_NETUTILS_NTP 未开），实际校时依赖墨墨接口的
 * HTTP Date 头，因此未配置墨墨 token 的机器时间恒为 1970、闹钟不会响。
 */
#ifndef __ALARM_H__
#define __ALARM_H__

#include <rtthread.h>

/* 响铃无人确认的超时（秒），到点自动静默 */
#define APP_ALARM_RING_TIMEOUT_S 180

/* UI 点屏停止响铃 */
void app_alarm_stop(void);

/* 保存某槽闹钟后调用：允许把时间改成当前分钟也能触发（清已触发标记） */
void app_alarm_invalidate_slot(int idx);

/* 标记闹钟表有改动待落盘。所有修改 g_app.alarms 的地方（UI 保存、开关、
 * 单次触发自动关、AI 工具）改完都必须调一次，否则重启后改动丢失。 */
void app_alarm_save_request(void);

#endif

