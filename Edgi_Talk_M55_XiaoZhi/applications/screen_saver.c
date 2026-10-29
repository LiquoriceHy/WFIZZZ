/*
 * screen_saver.c - 屏保自动息屏
 *
 * 判定方式：直接使用 LVGL 的 lv_display_get_inactive_time()。触摸按下与
 * 按下中都会在 lv_indev.c 中刷新 last_activity_time，因此任何触摸操作
 * （包括横滑翻页、点击闹钟卡片）都会自动视为"有活动"，无需在各控件中
 * 逐一挂钩子。
 *
 * 执行方式：lv_timer 每秒检查一次非活动时长，超过阈值即关闭背光；一旦
 * 检测到触摸（非活动时长回落到阈值内）立即恢复。该方式唤醒不依赖任何
 * 外部事件源，也不受小智客户端是否启用影响。
 *
 * 背光链路（与 board.c、main.c 一致）：
 *   P15.7 = 背光电源开关（HIGH 为亮，LOW 为灭；board.c 开机初始化时即拉低遮黑）
 *   P20.6 = 背光 PWM 使能，main.c 在首帧后拉高
 *   亮度由 drv_lcd.c 经 pwm18 设置，本模块不修改亮度，恢复后沿用原亮度。
 *
 * 关屏后 LVGL 仍照常渲染（只是不可见），触摸事件也照常投递，因此
 * 亮屏后界面内容已是最新，无需重绘或重建。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>       /* GET_PIN：与 main.c / app_data.c 同源 */
#include "lvgl.h"
#include "screen_saver.h"

#define LCD_BL_PIN                  GET_PIN(15, 7)
#define SCREEN_SAVER_TIMEOUT_MS     (60 * 1000)
#define SCREEN_SAVER_POLL_MS        1000

static volatile rt_bool_t _screen_off = RT_FALSE;
static rt_bool_t _inited = RT_FALSE;
static lv_timer_t *_poll_timer = RT_NULL;

static void screen_set(rt_bool_t on)
{
    rt_pin_write(LCD_BL_PIN, on ? PIN_HIGH : PIN_LOW);
    _screen_off = !on;
}

/* 1s 轮询：非活动时长超阈值关屏，回落后亮屏 */
static void _poll_cb(lv_timer_t *timer)
{
    uint32_t inactive_ms = lv_display_get_inactive_time(NULL);

    RT_UNUSED(timer);

    if (!_screen_off && inactive_ms >= SCREEN_SAVER_TIMEOUT_MS)
    {
        screen_set(RT_FALSE);
        rt_kprintf("[screen_saver] screen off (idle %u ms)\n", inactive_ms);
    }
    else if (_screen_off && inactive_ms < SCREEN_SAVER_TIMEOUT_MS)
    {
        /* 触摸发生：LVGL 刷新了 last_activity_time，非活动时长回落到阈值内 */
        screen_set(RT_TRUE);
        rt_kprintf("[screen_saver] screen on\n");
    }
}

void screen_saver_notify_activity(void)
{
    /* 兼容保留：触摸已由 LVGL 自动计入活动时间，这里额外提供
     * 主动亮屏通道（例如将来由按键/网络消息触发）。 */
    if (_inited && _screen_off)
    {
        screen_set(RT_TRUE);
    }
}

rt_bool_t screen_saver_is_screen_off(void)
{
    return _screen_off;
}

int screen_saver_init(void)
{
    /* 不在此处调用 rt_pin_mode：GPIO 输出模式已由 board.c 配置完成，且
     * PIN_MODE_OUTPUT 会将引脚初始电平置高，从而在 main() 打开背光之前
     * 点亮屏幕，露出开机过程中的无效画面。 */

    if (_inited)
        return 0;

    _poll_timer = lv_timer_create(_poll_cb, SCREEN_SAVER_POLL_MS, RT_NULL);
    if (_poll_timer == RT_NULL)
    {
        rt_kprintf("[screen_saver] lv_timer create failed\n");
        return -RT_ENOMEM;
    }

    _inited = RT_TRUE;
    rt_kprintf("[screen_saver] init ok, timeout %d ms\n", SCREEN_SAVER_TIMEOUT_MS);
    return 0;
}
