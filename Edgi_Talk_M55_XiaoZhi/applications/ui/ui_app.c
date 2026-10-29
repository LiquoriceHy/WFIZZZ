/*
 * ui_app.c - 应用全屏屏幕
 *
 * 布局（800x480）：
 *   root    全屏不透明容器，bg = UI_BG，盖住状态栏与 pager
 *   ├ bar   顶部条：Back 按钮 + 应用名 + 状态文字
 *   └ body  剩余区域，由应用的 draw 回调填内容
 *
 * 为什么用对象层而不是 lv_screen_load()：
 *   LVGL 只有一个活动屏幕，切换屏幕会销毁/重建控件树，pager 的
 *   滚动位置也丢。这里用全屏容器覆盖，隐藏即返回，代价最小。
 *
 * 刷新：屏幕打开期间每 500ms 调一次应用的 draw 回调（draw 负责
 * 按需更新自己的标签文本，不做重建）。同时续期 activity 防息屏。
 */
#include <rtthread.h>
#include <string.h>

#include "lvgl.h"
#include "ui_theme.h"
#include "ui_app.h"
#include "app_registry.h"

#define APP_BAR_H       56
#define APP_REFRESH_MS  500

static lv_obj_t *app_root   = RT_NULL;
static lv_obj_t *app_body   = RT_NULL;
static lv_obj_t *app_title  = RT_NULL;
static lv_obj_t *app_state  = RT_NULL;
static lv_timer_t *app_timer = RT_NULL;
static lv_obj_t *app_btn_start = RT_NULL;   /* 不可启动的应用隐藏它 */
static lv_obj_t *app_btn_start_txt = RT_NULL;

static int app_current = -1;
static rt_bool_t app_open = RT_FALSE;

/* ---------------- 动作 ---------------- */

static void app_back_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    ui_app_close();
}

static void app_start_stop_cb(lv_event_t *e)
{
    int count = 0;
    const app_entry_t *apps;
    const app_entry_t *a;
    rt_bool_t running;

    RT_UNUSED(e);

    if (app_current < 0)
        return;

    apps = app_registry_get(&count);
    if (app_current >= count)
        return;

    a = &apps[app_current];
    running = (a->is_running != RT_NULL) ? a->is_running() : RT_FALSE;

    if (running)
    {
        app_registry_stop(app_current);
    }
    else if (app_registry_start(app_current) != 0)
    {
        rt_kprintf("[app] start %d failed\n", app_current);
    }
}

/* ---------------- 周期刷新 ---------------- */

static void app_timer_cb(lv_timer_t *t)
{
    int count = 0;
    const app_entry_t *apps;
    const app_entry_t *a;
    rt_bool_t running;

    RT_UNUSED(t);

    if (!app_open || app_current < 0)
        return;

    /* 应用屏打开期间保持亮屏 */
    lv_display_trigger_activity(NULL);

    apps = app_registry_get(&count);
    if (app_current >= count)
        return;

    a = &apps[app_current];
    running = (a->is_running != RT_NULL) ? a->is_running() : RT_FALSE;

    if (app_state != RT_NULL)
    {
        ui_label_set_text_if_diff(app_state, running ? "RUNNING" : "IDLE");
        ui_set_text_color_if_diff(app_state, running ? UI_INK : UI_TEXT_DIM);
    }

    /* 只读应用（无 start）不显示操作按钮 */
    if (app_btn_start != RT_NULL)
    {
        if (a->start == RT_NULL)
        {
            if (!lv_obj_has_flag(app_btn_start, LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(app_btn_start, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            if (lv_obj_has_flag(app_btn_start, LV_OBJ_FLAG_HIDDEN))
                lv_obj_clear_flag(app_btn_start, LV_OBJ_FLAG_HIDDEN);
            if (app_btn_start_txt != RT_NULL)
                ui_label_set_text_if_diff(app_btn_start_txt,
                                          running ? "STOP" : "START");
        }
    }

    /* 内容由应用自己刷新（只改文本，不重建控件） */
    if (a->draw != RT_NULL)
        a->draw(app_body);
}

/* ---------------- 构建 ---------------- */

static lv_obj_t *bar_button(lv_obj_t *parent, const char *txt,
                            lv_coord_t w, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_t *label;

    lv_obj_set_size(btn, w, 36);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, RT_NULL);

    label = ui_label(btn, &ui_style_small);
    lv_label_set_text(label, txt);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_BG), 0);
    lv_obj_center(label);

    return btn;
}

void ui_app_create(lv_obj_t *scr)
{
    lv_obj_t *btn_back;
    lv_obj_t *btn_start;

    if (app_root != RT_NULL)
        return;

    /* 全屏不透明底：盖住状态栏、pager、页码点 */
    app_root = lv_obj_create(scr);
    lv_obj_set_size(app_root, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(app_root, 0, 0);
    lv_obj_set_style_bg_color(app_root, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(app_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(app_root, 0, 0);
    lv_obj_set_style_radius(app_root, 0, 0);
    lv_obj_set_style_pad_all(app_root, 0, 0);
    lv_obj_clear_flag(app_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(app_root, LV_OBJ_FLAG_HIDDEN);

    /* 顶部条：Back + 标题 + 状态 + START/STOP */
    btn_back = bar_button(app_root, "Back", 80, app_back_cb);
    lv_obj_set_pos(btn_back, UI_PAD, 10);

    app_title = ui_label(app_root, &ui_style_text);
    lv_label_set_text(app_title, "");
    lv_obj_set_pos(app_title, UI_PAD + 96, 18);

    app_state = ui_label(app_root, &ui_style_small);
    lv_label_set_text(app_state, "IDLE");
    lv_obj_set_width(app_state, 120);
    lv_obj_set_style_text_align(app_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(app_state, UI_SCREEN_W - UI_PAD - 120 - 120 - 12, 20);

    btn_start = bar_button(app_root, "START", 110, app_start_stop_cb);
    lv_obj_set_pos(btn_start, UI_SCREEN_W - UI_PAD - 110, 10);
    app_btn_start = btn_start;
    app_btn_start_txt = lv_obj_get_child(btn_start, 0);

    /* 分隔线 */
    {
        lv_obj_t *sep = lv_obj_create(app_root);

        lv_obj_set_size(sep, UI_SCREEN_W - UI_PAD * 2, 1);
        lv_obj_set_pos(sep, UI_PAD, APP_BAR_H);
        lv_obj_set_style_bg_color(sep, lv_color_hex(UI_SEP), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sep, 0, 0);
        lv_obj_set_style_radius(sep, 0, 0);
        lv_obj_set_style_pad_all(sep, 0, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);
    }

    /* 内容区：应用的 draw 往这里画 */
    app_body = lv_obj_create(app_root);
    lv_obj_set_size(app_body, UI_SCREEN_W, UI_SCREEN_H - APP_BAR_H - 2);
    lv_obj_set_pos(app_body, 0, APP_BAR_H + 2);
    lv_obj_set_style_bg_opa(app_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(app_body, 0, 0);
    lv_obj_set_style_radius(app_body, 0, 0);
    lv_obj_set_style_pad_all(app_body, UI_PAD, 0);
    lv_obj_clear_flag(app_body, LV_OBJ_FLAG_SCROLLABLE);

    app_timer = lv_timer_create(app_timer_cb, APP_REFRESH_MS, RT_NULL);
    lv_timer_pause(app_timer);
}

void ui_app_open(int index)
{
    int count = 0;
    const app_entry_t *apps;

    apps = app_registry_get(&count);
    if (index < 0 || index >= count || app_root == RT_NULL)
        return;

    /* 已有屏幕时先让旧应用把内容清掉 */
    if (app_open && app_current >= 0 && app_current < count &&
        apps[app_current].undraw != RT_NULL)
    {
        apps[app_current].undraw(app_body);
    }

    /* 控件实体统一删除，应用只需管自己的指针 */
    if (app_body != RT_NULL)
        lv_obj_clean(app_body);

    app_current = index;
    app_open = RT_TRUE;

    lv_label_set_text(app_title, apps[index].name);
    lv_obj_clear_flag(app_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(app_root);

    if (app_timer != RT_NULL)
        lv_timer_resume(app_timer);

    lv_display_trigger_activity(NULL);
    /* 立即刷一次：状态文字 + 内容首建（app_timer_cb 内部会调 draw，
     * 不在这里再单独调一遍，避免打开时 draw 跑两次） */
    app_timer_cb(RT_NULL);
}

void ui_app_close(void)
{
    int count = 0;
    const app_entry_t *apps = app_registry_get(&count);

    if (app_current >= 0 && app_current < count &&
        apps[app_current].undraw != RT_NULL && app_body != RT_NULL)
    {
        apps[app_current].undraw(app_body);
    }

    /* undraw 只负责清应用自己的指针；控件实体在这里统一删掉，
     * 否则下次进入同一应用会往旧控件上叠加 */
    if (app_body != RT_NULL)
        lv_obj_clean(app_body);

    app_current = -1;
    app_open = RT_FALSE;

    if (app_timer != RT_NULL)
        lv_timer_pause(app_timer);
    if (app_root != RT_NULL)
        lv_obj_add_flag(app_root, LV_OBJ_FLAG_HIDDEN);
}

rt_bool_t ui_app_is_open(void)
{
    return app_open;
}

int ui_app_current(void)
{
    return app_current;
}
