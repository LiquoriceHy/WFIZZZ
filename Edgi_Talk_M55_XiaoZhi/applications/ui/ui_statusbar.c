/*
 * ui_statusbar.c - 顶部状态栏：WiFi 信号 | MQTT 指示 | 电池 | 右侧 IP
 *
 * 与页面同底色，只用底部一条发丝线界定，避免顶部出现深色横条。
 * 颜色语义：链路正常=墨黑，链路断开=橙（需要关注），未接入功能=暖灰。
 * MQTT 由 snore_rt 的 paho 连接状态驱动（app_data_set_mqtt），snore 应用
 * 未启动时为暖灰而非橙色告警。电池由 app_data 本地采样（约 30s
 * 一次），无数据=暖灰，低电（<3500mV 或 <20%）=橙。
 *
 * 数据来自 g_app（app_data 线程每秒刷新）；本模块再建一个 1s lv_timer
 * 把 volatile 值搬到控件上。
 */
#include <rtthread.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "app_data.h"

static lv_obj_t *wifi_icon;
static lv_obj_t *wifi_text;
static lv_obj_t *mqtt_dot;
static lv_obj_t *mqtt_text;
static lv_obj_t *res_text;
static lv_obj_t *batt_icon;
static lv_obj_t *batt_text;
static lv_obj_t *ip_text;

void ui_statusbar_refresh(void)
{
    if (g_app.wifi_connected)
    {
        ui_set_text_color_if_diff(wifi_icon, UI_INK);
        ui_set_text_color_if_diff(wifi_text, UI_INK);
        if (g_app.wifi_valid)
            ui_label_set_text_fmt_if_diff(wifi_text, "%d dBm", g_app.wifi_rssi);
        else
            ui_label_set_text_if_diff(wifi_text, "Connected");
    }
    else
    {
        ui_set_text_color_if_diff(wifi_icon, UI_ATTENTION);
        ui_set_text_color_if_diff(wifi_text, UI_ATTENTION);
        ui_label_set_text_if_diff(wifi_text, "Offline");
    }

    /* MQTT：在线墨黑，未接入暖灰（不是告警） */
    ui_set_bg_color_if_diff(mqtt_dot, g_app.mqtt_online ? UI_INK : UI_MUTED);
    ui_set_text_color_if_diff(mqtt_text, g_app.mqtt_online ? UI_INK : UI_TEXT_DIM);

    /* 资源占用：CPU 来自 cpu_usage 包，MEM 来自 RT-Thread 堆，均为占用百分比 */
    ui_label_set_text_fmt_if_diff(res_text, "CPU %u%%  MEM %u%%",
                                  (unsigned int)g_app.cpu_used_pct,
                                  (unsigned int)g_app.mem_used_pct);

    /* 电池：按电量挡位选图标；低电橙色告警，无数据暖灰占位 */
    if (g_app.batt_valid)
    {
        const char *sym;

        if (g_app.batt_pct >= 90)      sym = LV_SYMBOL_BATTERY_FULL;
        else if (g_app.batt_pct >= 65) sym = LV_SYMBOL_BATTERY_3;
        else if (g_app.batt_pct >= 40) sym = LV_SYMBOL_BATTERY_2;
        else if (g_app.batt_pct >= 20) sym = LV_SYMBOL_BATTERY_1;
        else                           sym = LV_SYMBOL_BATTERY_EMPTY;

        if (g_app.batt_mv < 3500 || g_app.batt_pct < 20)
        {
            ui_set_text_color_if_diff(batt_icon, UI_ATTENTION);
            ui_set_text_color_if_diff(batt_text, UI_ATTENTION);
        }
        else
        {
            ui_set_text_color_if_diff(batt_icon, UI_INK);
            ui_set_text_color_if_diff(batt_text, UI_INK);
        }

        ui_label_set_text_if_diff(batt_icon, sym);
        ui_label_set_text_fmt_if_diff(batt_text, "%u%%", (unsigned int)g_app.batt_pct);
    }
    else
    {
        ui_label_set_text_if_diff(batt_icon, LV_SYMBOL_BATTERY_EMPTY);
        ui_label_set_text_if_diff(batt_text, "--");
        ui_set_text_color_if_diff(batt_icon, UI_OFFLINE);
        ui_set_text_color_if_diff(batt_text, UI_OFFLINE);
    }

    ui_label_set_text_if_diff(ip_text, (const char *)g_app.ip_str);
}

static void status_timer_cb(lv_timer_t *timer)
{
    RT_UNUSED(timer);
    ui_statusbar_refresh();
}

lv_obj_t *ui_statusbar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_t *rule;

    lv_obj_set_size(bar, UI_SCREEN_W, UI_BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_BAR_BG), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);

    /* 底部发丝线：界定状态栏，同时作为内容区的上边界 */
    rule = lv_obj_create(bar);
    lv_obj_set_size(rule, UI_SCREEN_W, 1);
    lv_obj_align(rule, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(rule, lv_color_hex(UI_SEP), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(rule, 0, 0);
    lv_obj_set_style_radius(rule, 0, 0);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);

    /* WiFi：图标 + 信号（固定偏移定位：布局前 align_to 不可靠） */
    wifi_icon = ui_label(bar, &ui_style_small);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_align(wifi_icon, LV_ALIGN_LEFT_MID, UI_PAD, 0);

    wifi_text = ui_label(bar, &ui_style_small);
    lv_label_set_text(wifi_text, "Offline");
    lv_obj_align(wifi_text, LV_ALIGN_LEFT_MID, UI_PAD + 28, 0);

    /* MQTT 指示：6x6 直角小方块 + 文字 */
    mqtt_dot = lv_obj_create(bar);
    lv_obj_set_size(mqtt_dot, 6, 6);
    lv_obj_align(mqtt_dot, LV_ALIGN_LEFT_MID, 150, 0);
    lv_obj_set_style_radius(mqtt_dot, 0, 0);
    lv_obj_set_style_border_width(mqtt_dot, 0, 0);
    lv_obj_clear_flag(mqtt_dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mqtt_dot, LV_OBJ_FLAG_CLICKABLE);

    mqtt_text = ui_label(bar, &ui_style_small_dim);
    lv_label_set_text(mqtt_text, "MQTT");
    lv_obj_align(mqtt_text, LV_ALIGN_LEFT_MID, 164, 0);

    /* 资源占用（原 LVGL 左下角悬浮框的信息，移到这里，跟随状态栏主题） */
    res_text = ui_label(bar, &ui_style_small_dim);
    lv_label_set_text(res_text, "CPU --%  MEM --%");
    lv_obj_align(res_text, LV_ALIGN_LEFT_MID, 260, 0);

    /* 电池：图标 + 百分比（数据由 app_data 约 30s 采样一次；空档 400~670 内取 420） */
    batt_icon = ui_label(bar, &ui_style_small);
    lv_label_set_text(batt_icon, LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_align(batt_icon, LV_ALIGN_LEFT_MID, 420, 0);

    batt_text = ui_label(bar, &ui_style_small_dim);
    lv_label_set_text(batt_text, "--");
    lv_obj_align(batt_text, LV_ALIGN_LEFT_MID, 446, 0);

    /* 右侧 IP */
    ip_text = ui_label(bar, &ui_style_small_dim);
    lv_label_set_text(ip_text, "--");
    lv_obj_align(ip_text, LV_ALIGN_RIGHT_MID, -UI_PAD, 0);

    lv_timer_create(status_timer_cb, 1000, NULL);
    ui_statusbar_refresh();

    return bar;
}
