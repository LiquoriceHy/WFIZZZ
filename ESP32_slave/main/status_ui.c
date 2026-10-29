/* 状态屏绘制与刷新。只通过各模块的只读接口取状态
 *（wifi_sta/mqtt_link/led_ctrl），不持有外部状态。 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "ST7789.h"            /* 板级屏驱动（含 LVGL_Driver.h + lvgl.h） */
#include "app_config.h"
#include "wifi_sta.h"
#include "mqtt_link.h"
#include "led_ctrl.h"
#include "status_ui.h"

/* cmd/reply 层写 / lvgl 任务读，字符串进临界区 */
static portMUX_TYPE s_stat_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_disp_cmd[64];
static char s_disp_reply[104];

/* 240x240 布局：
 *   y   4  标题（font20）
 *   y  30  分隔线
 *   y  38  WiFi / IP
 *   y  78  MQTT / RSSI
 *   y 118  UP / LED
 *   y 152  分隔线
 *   y 160  最近命令（>）与回包（<，自动换行，屏底截断）
 */
static lv_obj_t *ui_wifi, *ui_ip, *ui_mqtt, *ui_rssi;
static lv_obj_t *ui_up, *ui_led;
static lv_obj_t *ui_cmd, *ui_reply;

static void ui_separator(lv_obj_t *parent, int y)
{
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_set_size(line, 228, 1);
    lv_obj_set_pos(line, 6, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(0x404040), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *ui_line_label(lv_obj_t *parent, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, 8, y);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE0E0E0), 0);
    lv_label_set_text(l, "-");
    return l;
}

static void ui_build(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_pos(title, 8, 4);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(title, DEV_ID " LAN DEV");

    ui_separator(scr, 30);

    ui_wifi = ui_line_label(scr, 38);
    ui_ip   = ui_line_label(scr, 58);
    ui_mqtt = ui_line_label(scr, 78);
    ui_rssi = ui_line_label(scr, 98);
    ui_up   = ui_line_label(scr, 118);
    ui_led  = ui_line_label(scr, 138);

    ui_separator(scr, 160);

    ui_cmd = lv_label_create(scr);
    lv_obj_set_pos(ui_cmd, 8, 166);
    lv_obj_set_width(ui_cmd, 226);
    lv_obj_set_style_text_font(ui_cmd, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui_cmd, lv_color_hex(0x9FC5B4), 0);
    lv_label_set_long_mode(ui_cmd, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ui_cmd, "> -");

    ui_reply = lv_label_create(scr);
    lv_obj_set_pos(ui_reply, 8, 192);
    lv_obj_set_width(ui_reply, 226);
    lv_obj_set_style_text_font(ui_reply, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ui_reply, lv_color_hex(0xE8C58A), 0);
    lv_label_set_long_mode(ui_reply, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ui_reply, "< -");
}

static void ui_update(void)
{
    char buf[112];
    char ip_str[16];
    int rssi;
    uint64_t up_s;

    rssi = wifi_sta_get_rssi();
    if (wifi_sta_api_ready())
        wifi_sta_get_ip(ip_str, sizeof(ip_str));
    else
        strlcpy(ip_str, "-", sizeof(ip_str));
    up_s = esp_timer_get_time() / 1000000ULL;

    if (!wifi_sta_api_ready())
        lv_label_set_text(ui_wifi, "WiFi: starting...");
    else if (wifi_sta_is_up())
        lv_label_set_text_fmt(ui_wifi, "WiFi: %s", WIFI_SSID);
    else
    {
        /* 带断连原因码：201=找不到 AP，2/15=密码错，205=安全类型不支持 */
        snprintf(buf, sizeof(buf), "WiFi: conn (%d)", wifi_sta_last_reason());
        lv_label_set_text(ui_wifi, buf);
    }
    lv_label_set_text_fmt(ui_ip, "IP: %s", ip_str);
    lv_label_set_text_fmt(ui_mqtt, "MQTT: %s",
                          mqtt_link_is_up() ? "online" : "offline");
    if (wifi_sta_api_ready())
        lv_label_set_text_fmt(ui_rssi, "RSSI: %d dBm", rssi);
    else
        lv_label_set_text(ui_rssi, "RSSI: -");
    lv_label_set_text_fmt(ui_up, "UP: %02u:%02u:%02u",
                          (unsigned)(up_s / 3600),
                          (unsigned)(up_s / 60) % 60,
                          (unsigned)up_s % 60);
    lv_label_set_text_fmt(ui_led, "LED: %s",
                          led_ctrl_is_on() ? "ON" : "OFF");

    portENTER_CRITICAL(&s_stat_mux);
    strlcpy(buf, s_disp_cmd, sizeof(buf));
    portEXIT_CRITICAL(&s_stat_mux);
    lv_label_set_text_fmt(ui_cmd, "> %s", buf);

    portENTER_CRITICAL(&s_stat_mux);
    strlcpy(buf, s_disp_reply, sizeof(buf));
    portEXIT_CRITICAL(&s_stat_mux);
    lv_label_set_text_fmt(ui_reply, "< %s", buf);
}

static void lvgl_task(void *arg)
{
    bool built = false;
    uint32_t last_ms = 0;

    for (;;)
    {
        if (!built)
        {
            ui_build();          /* 首帧在此任务里建，之后只刷新 */
            built = true;
        }
        lv_timer_handler();

        uint32_t now = lv_tick_get();
        if (now - last_ms >= 500)
        {
            ui_update();
            last_ms = now;
        }
        /* 必须至少 10ms：本工程 FreeRTOS tick=100Hz，5ms 经
         * pdMS_TO_TICKS 整数截断为 0，vTaskDelay(0) 不休眠只让出，
         * 会把 IDLE/main 全饿死（看门狗报 CPU0: lvgl 即此因） */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void status_ui_start(void)
{
    LCD_Init();
    LVGL_Init();
    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 2, NULL);
}

void status_ui_set_cmd(const char *txt)
{
    portENTER_CRITICAL(&s_stat_mux);
    strlcpy(s_disp_cmd, txt, sizeof(s_disp_cmd));
    portEXIT_CRITICAL(&s_stat_mux);
}

void status_ui_set_reply(const char *txt)
{
    portENTER_CRITICAL(&s_stat_mux);
    strlcpy(s_disp_reply, txt, sizeof(s_disp_reply));
    portEXIT_CRITICAL(&s_stat_mux);
}
