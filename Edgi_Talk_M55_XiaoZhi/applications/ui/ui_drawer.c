/*
 * ui_drawer.c - 底部上滑应用抽屉
 *
 * 结构（屏幕坐标系 800x480）：
 *   root   全屏遮罩，bg 黑色 50% 透明，点空白关闭
 *   panel  800x340 底部面板，从 y=480 滑入到 y=140
 *          包含 header（"Apps" 标题与 Close 按钮）与
 *          grid（应用卡片，flex 行排自动换行）
 *
 * 卡片每个应用一张（来自 app_registry）：
 *   图标块（墨黑 44x44，取名字首字母）、名称、状态行、START/STOP
 *
 * 状态刷新：300ms 轮询。抽屉常驻（仅显隐），因此定时器持续运行但只在
 * 可见时更新；写控件经过 ui_theme 的 if_diff 守卫，值未变化时不重绘。
 *
 * 动画：LVGL 9.2 无 lv_obj_animate，使用 lv_anim_t 加自定义 exec 包裹
 * lv_obj_set_y。开合前先调用 lv_anim_delete 防止动画叠加。
 */
#include <rtthread.h>
#include <string.h>

#include "lvgl.h"
#include "ui_theme.h"
#include "ui_drawer.h"
#include "ui_app.h"
#include "ui_app_icons.h"
#include "app_registry.h"

#define DRW_PANEL_H       340
#define DRW_PANEL_Y0      UI_SCREEN_H              /* 收起时：完全在屏外 */
#define DRW_PANEL_Y1      (UI_SCREEN_H - DRW_PANEL_H)
#define DRW_ANIM_MS       220

#define DRW_CARD_W        244
#define DRW_CARD_H        132
#define DRW_GRID_PAD      16

#define DRW_MAX_APPS      8

static lv_obj_t *drw_root = RT_NULL;     /* 全屏遮罩 */
static lv_obj_t *drw_panel = RT_NULL;    /* 底部面板 */
static lv_obj_t *drw_grid = RT_NULL;     /* 应用网格（列表态） */
static lv_obj_t *drw_btn_txt[DRW_MAX_APPS];
static lv_obj_t *drw_sta_txt[DRW_MAX_APPS];

static lv_timer_t *drw_timer = RT_NULL;
static rt_bool_t drw_open = RT_FALSE;

static void drawer_timer_cb(lv_timer_t *t);

/* ---------------- 动画 ---------------- */

static void anim_set_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, v);
}

static void panel_slide_to(int32_t target_y, lv_anim_completed_cb_t done_cb)
{
    lv_anim_t a;

    /* 先删旧动画，避免连点导致位置跳变 */
    lv_anim_delete(drw_panel, anim_set_y_cb);

    lv_anim_init(&a);
    lv_anim_set_var(&a, drw_panel);
    lv_anim_set_exec_cb(&a, anim_set_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(drw_panel), target_y);
    lv_anim_set_duration(&a, DRW_ANIM_MS);
    lv_anim_set_path_cb(&a, target_y > lv_obj_get_y(drw_panel)
                             ? lv_anim_path_ease_in
                             : lv_anim_path_ease_out);
    if (done_cb != RT_NULL)
        lv_anim_set_completed_cb(&a, done_cb);
    lv_anim_start(&a);
}

static void close_done_cb(lv_anim_t *a)
{
    RT_UNUSED(a);
    if (!drw_open)                     /* 动画期间被重新打开则不隐藏 */
        lv_obj_add_flag(drw_root, LV_OBJ_FLAG_HIDDEN);
}

/* ---------------- 交互 ---------------- */

static void mask_clicked_cb(lv_event_t *e)
{
    /* 只有点在遮罩本身（不是面板/卡片）才收起 */
    if (lv_event_get_target(e) == drw_root)
        ui_drawer_close();
}

static void close_btn_cb(lv_event_t *e)
{
    RT_UNUSED(e);
    ui_drawer_close();
}

static void start_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (app_registry_start(idx) != 0)
        rt_kprintf("[drawer] start app %d failed\n", idx);
}

static void stop_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    app_registry_stop(idx);
}

/* 卡片点击：进入该应用的全屏屏幕并收起抽屉 */
static void card_clicked_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    ui_drawer_close();
    ui_app_open(idx);
}

/* ---------------- 状态刷新 ---------------- */

static void drawer_timer_cb(lv_timer_t *t)
{
    int count = 0, i;
    const app_entry_t *apps;

    RT_UNUSED(t);

    if (!drw_open)
        return;

    /* 抽屉打开期间保持屏幕常亮：只有触摸会刷新 activity，
     * 静置 60s 会被 screen_saver 关背光，这里周期性续期 */
    lv_display_trigger_activity(NULL);

    apps = app_registry_get(&count);
    if (count > DRW_MAX_APPS)
        count = DRW_MAX_APPS;

    for (i = 0; i < count; i++)
    {
        char st[48];
        rt_bool_t running = (apps[i].is_running != RT_NULL)
                                ? apps[i].is_running() : RT_FALSE;

        if (apps[i].get_status != RT_NULL)
        {
            st[0] = '\0';
            apps[i].get_status(st, sizeof(st));
        }
        else
        {
            rt_snprintf(st, sizeof(st), "%s", running ? "RUNNING" : "IDLE");
        }
        ui_label_set_text_if_diff(drw_sta_txt[i], st);

        /* 颜色：运行中墨黑，空闲暖灰 */
        ui_set_text_color_if_diff(drw_sta_txt[i], running ? UI_INK : UI_TEXT_DIM);

        if (drw_btn_txt[i] != RT_NULL)
            ui_label_set_text_if_diff(drw_btn_txt[i], running ? "STOP" : "START");
    }

}

/* ---------------- 构建 ---------------- */

/* 小按钮：墨黑底白字（主操作）或白底墨字描边。返回按钮对象本身。 */
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text,
                          lv_coord_t w, lv_coord_t h, rt_bool_t dark,
                          lv_event_cb_t cb, void *user)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_t *label;

    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    if (dark)
    {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    }
    else
    {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_CARD_BG), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_CARD_EDGE), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
    }

    label = ui_label(btn, &ui_style_small);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label,
        lv_color_hex(dark ? UI_CARD_BG : UI_INK), 0);
    lv_obj_center(label);

    if (cb != RT_NULL)
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);

    return btn;
}

static void build_app_cards(void)
{
    int count = 0, i;
    const app_entry_t *apps = app_registry_get(&count);

    if (count > DRW_MAX_APPS)
        count = DRW_MAX_APPS;

    /* 段池复位：图标里的独立线段在池中每次占一项，重建卡片时从头分配，
     * 避免反复建抽屉把池耗尽。 */
    ui_app_icon_reset();

    for (i = 0; i < count; i++)
    {
        lv_obj_t *card = ui_card_create(drw_grid, DRW_CARD_W, DRW_CARD_H);
        lv_obj_t *icon;
        lv_obj_t *name;
        lv_obj_t *desc;

        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        /* 点卡片任意空白处进详情；卡片内的按钮会先消费自己的点击，
         * 事件不会冒到这里 */
        lv_obj_add_event_cb(card, card_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        /* 图标：墨黑方块 + 线性图标。
         * APP_ICON_LETTER（或未登记的新应用）退回旧行为，画名字首字母。 */
        icon = lv_obj_create(card);
        lv_obj_set_size(icon, 40, 40);
        lv_obj_set_pos(icon, 12, 12);
        lv_obj_set_style_bg_color(icon, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(icon, 0, 0);
        lv_obj_set_style_radius(icon, 0, 0);
        lv_obj_set_style_pad_all(icon, 0, 0);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        ui_app_icon_draw(icon, apps[i].icon, UI_CARD_BG);

        if (apps[i].icon == APP_ICON_LETTER)
        {
            lv_obj_t *icon_txt = ui_label(icon, &ui_style_text);
            char c[2] = { apps[i].name[0], '\0' };

            lv_label_set_text(icon_txt, c);
            lv_obj_set_style_text_color(icon_txt, lv_color_hex(UI_CARD_BG), 0);
            lv_obj_center(icon_txt);
        }

        /* 名称 */
        name = ui_label(card, &ui_style_text);
        lv_label_set_text(name, apps[i].name);
        lv_obj_set_pos(name, 62, 14);

        /* 副标题 */
        desc = ui_label(card, &ui_style_small_dim);
        lv_label_set_text(desc, apps[i].desc);
        lv_obj_set_pos(desc, 62, 38);

        /* 状态行 */
        drw_sta_txt[i] = ui_label(card, &ui_style_small);
        lv_label_set_text(drw_sta_txt[i], "--");
        lv_obj_set_pos(drw_sta_txt[i], 12, 66);
        lv_obj_set_width(drw_sta_txt[i], DRW_CARD_W - 24);
        lv_obj_set_style_text_align(drw_sta_txt[i], LV_TEXT_ALIGN_LEFT, 0);

        /* 按钮：只读应用不画 */
        drw_btn_txt[i] = RT_NULL;
        if (apps[i].start != RT_NULL)
        {
            lv_obj_t *btn_start = make_btn(card, "START", 96, 30, RT_TRUE,
                                           start_cb, (void *)(intptr_t)i);
            lv_obj_align(btn_start, LV_ALIGN_BOTTOM_RIGHT, -12, -10);
            drw_btn_txt[i] = lv_obj_get_child(btn_start, 0);

            if (apps[i].stop != RT_NULL)
            {
                lv_obj_t *btn_stop = make_btn(card, "STOP", 72, 30, RT_FALSE,
                                              stop_cb, (void *)(intptr_t)i);
                lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_RIGHT, -116, -10);
            }
        }
    }
}

void ui_drawer_create(lv_obj_t *scr)
{
    lv_obj_t *title;
    lv_obj_t *btn_close;

    if (drw_root != RT_NULL)
        return;

    /* 遮罩：全屏、半透明黑、点击空白关闭 */
    drw_root = lv_obj_create(scr);
    lv_obj_set_size(drw_root, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(drw_root, 0, 0);
    lv_obj_set_style_bg_color(drw_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(drw_root, LV_OPA_50, 0);
    lv_obj_set_style_border_width(drw_root, 0, 0);
    lv_obj_set_style_radius(drw_root, 0, 0);
    lv_obj_set_style_pad_all(drw_root, 0, 0);
    lv_obj_clear_flag(drw_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(drw_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(drw_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(drw_root, mask_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* 面板：从屏外滑入 */
    drw_panel = lv_obj_create(drw_root);
    lv_obj_set_size(drw_panel, UI_SCREEN_W, DRW_PANEL_H);
    lv_obj_set_pos(drw_panel, 0, DRW_PANEL_Y0);
    lv_obj_set_style_bg_color(drw_panel, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(drw_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(drw_panel, 0, 0);
    lv_obj_set_style_radius(drw_panel, 0, 0);
    lv_obj_set_style_pad_all(drw_panel, 0, 0);
    lv_obj_clear_flag(drw_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部发丝线 + 标题 + Close */
    {
        lv_obj_t *rule = lv_obj_create(drw_panel);
        lv_obj_set_size(rule, UI_SCREEN_W, 1);
        lv_obj_set_pos(rule, 0, 0);
        lv_obj_set_style_bg_color(rule, lv_color_hex(UI_SEP), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rule, 0, 0);
        lv_obj_set_style_radius(rule, 0, 0);
        lv_obj_clear_flag(rule, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(rule, LV_OBJ_FLAG_CLICKABLE);
    }

    title = ui_label(drw_panel, &ui_style_value);
    lv_label_set_text(title, "Apps");
    lv_obj_set_pos(title, UI_PAD, 16);

    btn_close = make_btn(drw_panel, "Close", 88, 32, RT_FALSE,
                         close_btn_cb, RT_NULL);
    lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -UI_PAD, 14);

    /* 卡片网格：行排自动换行 */
    drw_grid = lv_obj_create(drw_panel);
    lv_obj_set_size(drw_grid, UI_SCREEN_W - DRW_GRID_PAD * 2,
                    DRW_PANEL_H - 68 - DRW_GRID_PAD);
    lv_obj_set_pos(drw_grid, DRW_GRID_PAD, 62);
    lv_obj_set_style_bg_opa(drw_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(drw_grid, 0, 0);
    lv_obj_set_style_pad_all(drw_grid, 0, 0);
    lv_obj_set_style_pad_row(drw_grid, 12, 0);
    lv_obj_set_style_pad_column(drw_grid, 12, 0);
    lv_obj_set_flex_flow(drw_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(drw_grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(drw_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(drw_grid, LV_SCROLLBAR_MODE_AUTO);

    build_app_cards();

    drw_timer = lv_timer_create(drawer_timer_cb, 300, RT_NULL);
}

void ui_drawer_open(void)
{
    if (drw_root == RT_NULL || drw_open)
        return;

    drw_open = RT_TRUE;
    lv_obj_clear_flag(drw_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(drw_root);          /* 压在状态栏/pager/圆点之上 */

    panel_slide_to(DRW_PANEL_Y1, RT_NULL);
    drawer_timer_cb(RT_NULL);                  /* 立即刷一次状态 */
}

void ui_drawer_close(void)
{
    if (drw_root == RT_NULL || !drw_open)
        return;

    drw_open = RT_FALSE;
    panel_slide_to(DRW_PANEL_Y0, close_done_cb);
}

rt_bool_t ui_drawer_is_open(void)
{
    return drw_open;
}
