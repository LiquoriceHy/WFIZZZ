/*
 * screen_saver.h - 屏保自动息屏接口
 *
 * 功能：1 分钟无触摸操作自动关闭 LCD 背光（P15.7），触摸后自动恢复。
 *
 * 活动判定：直接使用 LVGL 的 lv_display_get_inactive_time()。触摸按下与拖动
 * 都会刷新该值（lv_indev.c），因此横滑翻页、点击卡片、点击闹钟等任意触摸
 * 操作均自动视为活动，无需在各控件中挂载活动钩子。
 *
 * 背光链路：P15.7 为背光电源开关（HIGH 开，LOW 关）
 *           P20.6 为背光 PWM 使能（main.c 首帧后拉高）
 *           P15.6 为 LCD IC 电源（保持常开，避免重新初始化 DSI）
 *           亮度由 drv_lcd.c 经 pwm18 设置，本模块不介入
 */
#ifndef SCREEN_SAVER_H
#define SCREEN_SAVER_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 创建息屏轮询定时器。必须由 lv_user_gui_init 在 LVGL 初始化后调用
 * （依赖 lv_timer 与已注册的显示设备），重复调用安全。 */
int screen_saver_init(void);

/* 主动亮屏：触摸以外的活动源（如将来接回的按键或网络消息）可调用。
 * 纯触摸场景无需调用，因为 LVGL 已自动计入活动时间。 */
void screen_saver_notify_activity(void);

/* 查询当前是否处于息屏状态 */
rt_bool_t screen_saver_is_screen_off(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_SAVER_H */
