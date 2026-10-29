/*
 * ui_system.c - 系统页：完整系统信息（第二页，主页左滑进入）
 *
 * 表格化：键左对齐、值右对齐、行间全宽发丝线，不使用外框卡片，因为外框
 * 会把整页框成一个大盒子，与主页的裸排版语言不一致。
 * 颜色语义：正常值墨黑；离线或异常用橙 UI_ATTENTION；未接入用暖灰占位。
 *
 * 8 行信息：WiFi / IP 网关 / 时间同步 / MQTT / 运行时长 / 内存 / 电池 / 固件。
 *
 * 2026-10-26：WiFi 行可点击，进入屏上配网界面（ui_wifi）。
 * 该行右侧绘制 ">" 提示；热区刻意只覆盖右侧数值区（260px），
 * 不覆盖整行，因为整行加 ext_click_area 的命中面积过大，误触率高。
 */
#include <rtthread.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "app_data.h"
#include "ui_wifi.h"

#define SYS_ROW_NUM   8
#define SYS_ROW_H     48                           /* 8 行装入页面：由 54 改为 48（末行线 426，页高 444 余 18） */
#define SYS_TABLE_Y   54                           /* 标题与表格首条线之间 */
#define SYS_TABLE_W   (UI_SCREEN_W - UI_PAD * 2)   /* 752 */
#define SYS_LABEL_W   200

static lv_obj_t *row_value[SYS_ROW_NUM];

enum
{
    ROW_WIFI = 0,
    ROW_NET,
    ROW_SYNC,
    ROW_MQTT,
    ROW_UPTIME,
    ROW_MEM,
    ROW_BATT,
    ROW_FW
};

/* 点 WiFi 行的右侧数值区进入配网界面。
 *
 * 三道防误触措施（2026-10-26）：
 *   1) 热区从"整行 752px 加 ext 8px"收窄到右侧 260px 数值区。
 *   2) 事件使用 SHORT_CLICKED 而非 CLICKED：前者额外要求未发生长按，
 *      语义上即"轻点一下"，不会被长按路径重复触发。
 *   3) 总位移校验：pager 仅开启横向滚动（ui_pages.c），因此在该行上的
 *      竖向滑动不会命中任何滚动对象，LVGL 松手时仍会发送点击事件，
 *      原先在 WiFi 行上向下滑动即会进入配网界面。
 *
 * 不使用 lv_indev_get_press_moved() 的原因：该标志要求单帧位移
 * 超过 scroll_limit(10px) 才置位（lv_indev.c:1259）。缓慢拖动时每帧
 * 仅移动数像素，永远不会置位，慢速竖向滑动仍会误触。此处自行记录
 * 按下点，在松手时计算总位移，对快划与慢划采用一致判据。 */

#define SYS_WIFI_TAP_MAX_MOVE  12   /* 总位移超过 12px 视为滑动而非点击 */

static lv_point_t s_wifi_press_pt;
static rt_bool_t  s_wifi_press_valid;

static void sys_wifi_press_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;

    RT_UNUSED(e);

    s_wifi_press_valid = RT_FALSE;

    if (indev == RT_NULL)
        return;

    lv_indev_get_point(indev, &p);
    s_wifi_press_pt = p;
    s_wifi_press_valid = RT_TRUE;
}

static void sys_wifi_click_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    int32_t dx, dy;

    RT_UNUSED(e);

    if (indev == RT_NULL || !s_wifi_press_valid)
        return;

    s_wifi_press_valid = RT_FALSE;

    lv_indev_get_point(indev, &p);
    dx = (int32_t)p.x - (int32_t)s_wifi_press_pt.x;
    dy = (int32_t)p.y - (int32_t)s_wifi_press_pt.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    if (dx > SYS_WIFI_TAP_MAX_MOVE || dy > SYS_WIFI_TAP_MAX_MOVE)
        return;     /* 手指移动过，视为滑动 */

    ui_wifi_open();
}

/* ok=1 正常（墨黑）| ok=0 需要关注（橙）| ok=-1 未接入/无数据（暖灰） */
static void set_value_state(lv_obj_t *label, int ok)
{
    if (ok > 0)
        ui_set_text_color_if_diff(label, UI_INK);
    else if (ok == 0)
        ui_set_text_color_if_diff(label, UI_ATTENTION);
    else
        ui_set_text_color_if_diff(label, UI_OFFLINE);
}

static void sys_timer_cb(lv_timer_t *timer)
{
    RT_UNUSED(timer);

    /* WiFi */
    if (g_app.wifi_connected)
    {
        if (g_app.wifi_valid)
            ui_label_set_text_fmt_if_diff(row_value[ROW_WIFI], "Connected   %d dBm",
                                          g_app.wifi_rssi);
        else
            ui_label_set_text_if_diff(row_value[ROW_WIFI], "Connected");
        set_value_state(row_value[ROW_WIFI], 1);
    }
    else
    {
        ui_label_set_text_if_diff(row_value[ROW_WIFI], "Offline");
        set_value_state(row_value[ROW_WIFI], 0);
    }

    /* IP / 网关 */
    if (g_app.ip_str[0] != '-')
    {
        ui_label_set_text_fmt_if_diff(row_value[ROW_NET], "%s  /  %s",
                                      (const char *)g_app.ip_str,
                                      (const char *)g_app.gw_str);
        set_value_state(row_value[ROW_NET], 1);
    }
    else
    {
        ui_label_set_text_if_diff(row_value[ROW_NET], "no address");
        set_value_state(row_value[ROW_NET], -1);
    }

    /* 时间同步（NTP 预留） */
    ui_label_set_text_if_diff(row_value[ROW_SYNC],
                              g_app.ntp_synced ? "Synced" : "Not synced");
    set_value_state(row_value[ROW_SYNC], g_app.ntp_synced ? 1 : 0);

    /* MQTT：由 snore_rt 的 paho 连接状态驱动；未启动 snore 时 Offline 属于
     * "未启用"（暖灰）而非故障告警，与状态栏语义一致 */
    ui_label_set_text_if_diff(row_value[ROW_MQTT],
                              g_app.mqtt_online ? "Online" : "Offline");
    set_value_state(row_value[ROW_MQTT], g_app.mqtt_online ? 1 : -1);

    /* 运行时长 */
    ui_label_set_text_fmt_if_diff(row_value[ROW_UPTIME], "%ud %02u:%02u:%02u",
                                  (unsigned int)(g_app.uptime_s / 86400U),
                                  (unsigned int)((g_app.uptime_s % 86400U) / 3600U),
                                  (unsigned int)((g_app.uptime_s % 3600U) / 60U),
                                  (unsigned int)(g_app.uptime_s % 60U));
    set_value_state(row_value[ROW_UPTIME], 1);

    /* 内存 */
    ui_label_set_text_fmt_if_diff(row_value[ROW_MEM], "%u%%   free %u KB",
                                  g_app.mem_used_pct, (unsigned int)g_app.mem_free_kb);
    set_value_state(row_value[ROW_MEM], 1);

    /* 电池（ADC 本地采样，约 30s 更新一帧）：电压供核对换算，低电橙色 */
    if (g_app.batt_valid)
    {
        ui_label_set_text_fmt_if_diff(row_value[ROW_BATT], "%u.%02u V   %u%%",
                                      (unsigned int)(g_app.batt_mv / 1000U),
                                      (unsigned int)((g_app.batt_mv % 1000U) / 10U),
                                      (unsigned int)g_app.batt_pct);
        set_value_state(row_value[ROW_BATT],
                        (g_app.batt_mv < 3500 || g_app.batt_pct < 20) ? 0 : 1);
    }
    else
    {
        ui_label_set_text_if_diff(row_value[ROW_BATT], "no data");
        set_value_state(row_value[ROW_BATT], -1);
    }
}

lv_obj_t *ui_system_create(lv_obj_t *parent)
{
    static const char *const row_name[SYS_ROW_NUM] =
    {
        "WiFi", "IP / Gateway", "Time sync",
        "MQTT", "Uptime", "Memory", "Battery", "Firmware"
    };

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_t *title;
    int i;

    lv_obj_set_size(page, UI_SCREEN_W, UI_PAGE_H);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    /* 页标题：与主页时钟同处左侧基线，两页视觉同源 */
    title = ui_label(page, &ui_style_dim);
    lv_label_set_text(title, "System");
    lv_obj_set_pos(title, UI_PAD, 20);

    for (i = 0; i < SYS_ROW_NUM; i++)
    {
        /* 行底画全宽发丝线：末行线落在 426，页面 444，留 18px 底边距 */
        lv_coord_t y = (lv_coord_t)(SYS_TABLE_Y + i * SYS_ROW_H);

        lv_obj_t *name = ui_label(page, &ui_style_dim);
        lv_label_set_text(name, row_name[i]);
        lv_obj_set_pos(name, UI_PAD, (lv_coord_t)(y + 12));

        row_value[i] = ui_label(page, &ui_style_text);
        lv_obj_set_width(row_value[i], SYS_TABLE_W - SYS_LABEL_W);
        lv_obj_set_style_text_align(row_value[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(row_value[i], UI_PAD + SYS_LABEL_W, (lv_coord_t)(y + 12));

        /* WiFi 行：在右侧数值区叠加一个透明热区，点击进入配网界面。
         *
         * 2026-10-26 收窄：原为覆盖整行的 752×46，再加 ext_click_area(8)
         * 后变为 752×62，是全页最大的可点区域，误触率最高。现仅保留
         * 右侧"数值 + >"这一段（约 1/3 宽度），左侧 "WiFi" 键名不再
         * 响应，既保留"点数值进详情"的直觉，又大幅降低误触面积。
         * 同时移除 ext_click_area(8)，因为该调用会放大命中区，
         * 与防误触的目标相反。 */
        if (i == ROW_WIFI)
        {
            const lv_coord_t hit_w = 260;    /* 数值区 + ">" 提示符 */
            lv_obj_t *hit = lv_obj_create(page);
            lv_obj_t *chev;

            lv_obj_set_size(hit, hit_w, SYS_ROW_H - 4);
            lv_obj_set_pos(hit, UI_SCREEN_W - UI_PAD - hit_w,
                           (lv_coord_t)(y + 2));
            lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(hit, 0, 0);
            lv_obj_set_style_radius(hit, 0, 0);
            lv_obj_set_style_pad_all(hit, 0, 0);
            lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
            /* PRESSED 记录按下点，SHORT_CLICKED 时算总位移 */
            lv_obj_add_event_cb(hit, sys_wifi_press_cb, LV_EVENT_PRESSED, RT_NULL);
            lv_obj_add_event_cb(hit, sys_wifi_click_cb, LV_EVENT_SHORT_CLICKED, RT_NULL);
            lv_obj_move_foreground(hit);

            /* ">" 提示：告诉用户这行可以点。
             * 用 ASCII '>' 而非 LV_SYMBOL_RIGHT：后者在 montserrat 里
             * 需要 FontAwesome 段，虽然可用但与全页 ASCII 风格不一致。 */
            chev = ui_label(page, &ui_style_small_dim);
            lv_label_set_text(chev, ">");
            lv_obj_set_pos(chev, UI_SCREEN_W - UI_PAD - 10, (lv_coord_t)(y + 13));
            lv_obj_clear_flag(chev, LV_OBJ_FLAG_CLICKABLE);

            /* 数值右界左移，给 ">" 留位 */
            lv_obj_set_width(row_value[i], SYS_TABLE_W - SYS_LABEL_W - 16);
        }

        /* 行底画全宽发丝线，末行线落在 y=426，页面高 444，余 18px 底边距 */
        lv_obj_t *sep = lv_obj_create(page);
        lv_obj_set_size(sep, SYS_TABLE_W, 1);
        lv_obj_set_pos(sep, UI_PAD, (lv_coord_t)(y + SYS_ROW_H - 12));
        lv_obj_set_style_bg_color(sep, lv_color_hex(UI_SEP), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sep, 0, 0);
        lv_obj_set_style_radius(sep, 0, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE);
    }

    /* 固件行静态内容：RT-Thread 版本 + LVGL 版本 */
    lv_label_set_text_fmt(row_value[ROW_FW], "RT-Thread v%d.%d.%d   LVGL %d.%d.%d",
                          RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH,
                          LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH);
    set_value_state(row_value[ROW_FW], 1);

    lv_timer_create(sys_timer_cb, 1000, NULL);
    sys_timer_cb(NULL);

    return page;
}
