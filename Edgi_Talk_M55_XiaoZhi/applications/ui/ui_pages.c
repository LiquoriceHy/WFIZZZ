/*
 * ui_pages.c - 页面框架：固定状态栏 + 横向滑动两页 + 页码点 + 自动回主页
 *
 * 滑动用 LVGL 原生横向滚动（flex 行排页面 + 居中吸附），无需手势算法；
 * 30 秒无触摸操作自动滚回主页（lv_display_get_inactive_time 判断）。
 *
 * 2026-10-20 恢复：由归档的横滑三页版裁剪而来。语音页（ui_voice）依赖
 * 已退役的 voice_rec/voice_asr/agent_http 链路，且 app_data 的 voice_*
 * 字段已被 xiaozhi_* 聊天通道取代，故本次只恢复主页 + 系统页两页。
 * 语音链路若要回来，需连同那三个模块与字段一起恢复。
 */
#include <rtthread.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_statusbar.h"
#include "ui_dashboard.h"
#include "ui_system.h"
#include "ui_alarm.h"
#include "ui_drawer.h"
#include "ui_app.h"
#include "ui_wifi.h"

#define UI_AUTO_HOME_MS   30000
#define UI_PAGE_NUM       2
#define UI_DOT_GAP        6      /* 页点间距：16 宽短线加 6 间隔，中心距 22 */

static lv_obj_t *pager;        /* 横向滑动容器 */
static lv_obj_t *dot[UI_PAGE_NUM];

/* 前置声明：定义在文件下方，auto_home_timer_cb 先用到 */
static rt_bool_t fullscreen_layer_active(void);

static int pager_page(void)
{
    lv_coord_t x = lv_obj_get_scroll_x(pager);

    if (x > (lv_coord_t)UI_SCREEN_W / 2)
        return 1;
    return 0;
}

static void pager_scroll_cb(lv_event_t *e)
{
    static int last_page = -1;   /* 仅翻页时刷新圆点，避免每帧样式失效 */
    int page = pager_page();
    int i;

    RT_UNUSED(e);

    if (page == last_page)
        return;
    last_page = page;

    for (i = 0; i < UI_PAGE_NUM; i++)
    {
        lv_obj_set_style_bg_color(dot[i],
                                  lv_color_hex(page == i ? UI_INK : UI_MUTED), 0);
    }
}

static void auto_home_timer_cb(lv_timer_t *timer)
{
    RT_UNUSED(timer);

    /* 全屏层（app 屏 / WiFi 界面）盖着的时候不要偷偷把 pager 滚回主页：
     * 用户看不到这个动作，退出时会莫名其妙落在主页而不是原来的系统页。
     * 息屏由各全屏层自己的 lv_display_trigger_activity 维持。 */
    if (fullscreen_layer_active())
        return;

    if (pager_page() != 0 &&
        lv_display_get_inactive_time(NULL) >= UI_AUTO_HOME_MS)
    {
        lv_obj_scroll_to_x(pager, 0, LV_ANIM_ON);
    }
}

/* ---------------- 应用抽屉：底部上滑热区 ---------------- */

#define DRAWER_HOTZONE_H  36

/*
 * 热区：底部 36px 透明条，专门接收上滑手势。
 *
 * 不将 LV_EVENT_GESTURE 挂在整屏的原因：页面中可能存在纵向可滚动的控件
 * （roller/列表），在其上起手会被识别为滚动，手势事件被 scroll_obj 捕获
 * （lv_indev.c:1564）。底部热区是独立对象且无滚动，起手稳定。
 *
 * 方向：屏幕坐标 y 向下增长，手指上滑使 y 减小，因此为 LV_DIR_TOP。
 */
/* 全屏层是否已占据整屏。此时上滑不应再拉出抽屉，否则抽屉会覆盖到
 * WiFi 配网界面之上，用户既看不到也点不到自己的列表。
 * app 屏与 WiFi 界面均为全屏层，两者都要判断。 */
static rt_bool_t fullscreen_layer_active(void)
{
    return (ui_app_is_open() || ui_wifi_is_open()) ? RT_TRUE : RT_FALSE;
}

static void hotzone_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_dir_t dir;

    if (indev == RT_NULL)
        return;

    /* 应用屏幕 / WiFi 界面占满整屏，此时上滑不该再拉抽屉 */
    if (fullscreen_layer_active())
        return;

    dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_TOP)
        ui_drawer_open();

    RT_UNUSED(e);
}

/* 点一下热区也能开（照顾不习惯滑动的场景，且便于真机调试） */
static void hotzone_click_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    if (fullscreen_layer_active())
        return;

    ui_drawer_open();
}

static void drawer_hotzone_create(lv_obj_t *scr)
{
    lv_obj_t *hz = lv_obj_create(scr);

    lv_obj_set_size(hz, UI_SCREEN_W, DRAWER_HOTZONE_H);
    lv_obj_align(hz, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(hz, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hz, 0, 0);
    lv_obj_set_style_radius(hz, 0, 0);
    lv_obj_set_style_pad_all(hz, 0, 0);
    lv_obj_clear_flag(hz, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(hz, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(hz, 8);       /* 边缘 8px 也算命中，手指好按 */

    lv_obj_add_event_cb(hz, hotzone_gesture_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_event_cb(hz, hotzone_click_cb, LV_EVENT_CLICKED, NULL);

    /* 压在页码点之上，保证底部条带区域能被命中 */
    lv_obj_move_foreground(hz);
}

/* 页面通用底板：800x444、不滚动（手势透给 pager） */static lv_obj_t *page_base_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);

    lv_obj_set_size(page, UI_SCREEN_W, UI_PAGE_H);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    return page;
}

void ui_pages_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *page_home, *page_system;
    int i;

    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG), 0);

    /* 状态栏固定顶部，不参与滑动 */
    ui_statusbar_create(scr);

    /* 页面容器：横向滚动 + 居中吸附 */
    pager = lv_obj_create(scr);
    lv_obj_set_size(pager, UI_SCREEN_W, UI_PAGE_H);
    lv_obj_align(pager, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(pager, 0, 0);
    lv_obj_set_style_pad_column(pager, 0, 0);
    lv_obj_set_scroll_dir(pager, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(pager, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(pager, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pager, 0, 0);
    lv_obj_add_event_cb(pager, pager_scroll_cb, LV_EVENT_SCROLL, NULL);

    page_home = page_base_create(pager);
    ui_dashboard_create(page_home);

    page_system = page_base_create(pager);
    ui_system_create(page_system);

    /* 页码指示：底部中央发丝短线（当前页墨黑、其余浅灰） */
    for (i = 0; i < UI_PAGE_NUM; i++)
    {
        dot[i] = lv_obj_create(scr);
        lv_obj_set_size(dot[i], 16, 2);
        lv_obj_align(dot[i], LV_ALIGN_BOTTOM_MID,
                     (i - (UI_PAGE_NUM - 1) / 2) * (16 + UI_DOT_GAP), -8);
        lv_obj_set_style_radius(dot[i], 0, 0);
        lv_obj_set_style_border_width(dot[i], 0, 0);
        lv_obj_set_style_bg_color(dot[i],
                                  lv_color_hex(i == 0 ? UI_INK : UI_MUTED), 0);
        lv_obj_clear_flag(dot[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(dot[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* 响铃遮罩：最后创建压住全部页面，触发时再 move_foreground 兜底 */
    ui_alarm_ring_create(scr);

    /* 应用抽屉（隐藏态）+ 底部上滑热区。
     * 热区必须最后创建并 move_foreground，否则被 pager 盖住收不到触摸。 */
    ui_drawer_create(scr);
    drawer_hotzone_create(scr);

    /* 应用全屏屏幕：最后创建，打开时 move_foreground 覆盖一切。
     * 必须在抽屉之后创建，否则从抽屉点进 app 时会被抽屉遮罩压住。 */
    ui_app_create(scr);

    /* 屏上 WiFi 配网界面：比 app 屏更晚创建，因其从系统页进入，
     * 需要覆盖状态栏、pager、页码点、抽屉与 app 屏。打开时还会再
     * 调用 move_foreground 一次作为兜底。 */
    ui_wifi_create(scr);

    lv_timer_create(auto_home_timer_cb, 1000, NULL);
}
