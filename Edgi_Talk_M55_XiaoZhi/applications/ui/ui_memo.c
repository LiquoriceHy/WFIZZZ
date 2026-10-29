/*
 * ui_memo.c - 主页墨墨背单词卡片：今日进度 + 认识/模糊/忘记三色条
 *
 * 数据来自 g_app.memo（由 maimemo.c 线程写入），本文件只读，不访问网络。
 * 三色条本体无文字、无图例；条底色 UI_SEP 作为空轨道，无数据时读作"空条"。
 * 卡片内部使用纵向 flex 而非绝对坐标：ui_theme.h 的 UI_FONT_VALUE 在
 * lv_conf.h 开启 32 号字后会自动升字号，flex 会随之下排，不会互相重叠。
 */
#include <rtthread.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "app_data.h"

#define MEMO_CARD_W  168
#define MEMO_CARD_H  168
#define MEMO_PAD     16
#define MEMO_BAR_W   (MEMO_CARD_W - 2 * MEMO_PAD)   /* 136，与卡内内容区同宽 */
#define MEMO_BAR_H   12

enum { MEMO_SEG_KNOWN = 0, MEMO_SEG_FUZZY, MEMO_SEG_FORGET, MEMO_SEG_N };

static lv_obj_t *memo_value;
static lv_obj_t *memo_note;
static lv_obj_t *memo_seg[MEMO_SEG_N];

/* 三段宽度按计数比例分配，最后一段承接整除余量；计数为 0 的段隐藏。
 * 入口处比较计数值：数据数十秒才变化一次，值未变则不修改任何几何属性 */
static void memo_bar_update(rt_uint16_t known, rt_uint16_t fuzzy, rt_uint16_t forgot)
{
    static rt_uint16_t last[3] = {0xFFFF, 0xFFFF, 0xFFFF};
    int n[MEMO_SEG_N];
    int total = known + fuzzy + forgot;
    lv_coord_t x = 0;
    int i;

    if (last[0] == known && last[1] == fuzzy && last[2] == forgot)
        return;
    last[0] = known;
    last[1] = fuzzy;
    last[2] = forgot;

    n[MEMO_SEG_KNOWN] = known;
    n[MEMO_SEG_FUZZY] = fuzzy;
    n[MEMO_SEG_FORGET] = forgot;

    for (i = 0; i < MEMO_SEG_N; i++)
    {
        lv_coord_t w;

        if (total <= 0)
            w = 0;
        else if (i == MEMO_SEG_FORGET)
            w = MEMO_BAR_W - x;
        else
            w = (lv_coord_t)(MEMO_BAR_W * n[i] / total);

        lv_obj_set_pos(memo_seg[i], x, 0);
        lv_obj_set_size(memo_seg[i], w, MEMO_BAR_H);
        if (w > 0)
            lv_obj_clear_flag(memo_seg[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(memo_seg[i], LV_OBJ_FLAG_HIDDEN);

        x += w;
    }
}

static void memo_timer_cb(lv_timer_t *timer)
{
    rt_uint8_t  state  = g_app.memo.state;
    rt_uint16_t done   = g_app.memo.done;
    rt_uint16_t total  = g_app.memo.total;
    rt_uint16_t known  = g_app.memo.known;
    rt_uint16_t fuzzy  = g_app.memo.fuzzy;
    rt_uint16_t forgot = g_app.memo.forgot;
    const char *note = "error";
    uint32_t value_rgb = UI_OFFLINE;
    uint32_t note_rgb  = UI_TEXT_DIM;

    RT_UNUSED(timer);

    switch (state)
    {
    case APP_DATA_MEMO_OK:
        ui_label_set_text_fmt_if_diff(memo_value, "%u / %u",
                                      (unsigned int)done, (unsigned int)total);
        value_rgb = UI_INK;
        note = "done today";
        break;
    case APP_DATA_MEMO_WAIT:
        note = "syncing";
        break;
    case APP_DATA_MEMO_OFFLINE:
        note = "offline";
        break;
    case APP_DATA_MEMO_TOKEN:            /* 唯一需要用户处理的态：橙 */
        note = "token expired";
        note_rgb = UI_ATTENTION;
        break;
    case APP_DATA_MEMO_SYNC:
        note = "sync off";
        break;
    default:
        note = "error";
        break;
    }

    if (state != APP_DATA_MEMO_OK)
        ui_label_set_text_if_diff(memo_value, "--");

    ui_set_text_color_if_diff(memo_value, value_rgb);
    ui_label_set_text_if_diff(memo_note, note);
    ui_set_text_color_if_diff(memo_note, note_rgb);

    memo_bar_update(known, fuzzy, forgot);
}

/* 不参与布局的弹性占位，把三色条压到卡片底部 */
static lv_obj_t *flex_spacer(lv_obj_t *parent)
{
    lv_obj_t *sp = lv_obj_create(parent);

    lv_obj_set_width(sp, LV_PCT(100));
    lv_obj_set_height(sp, 0);
    lv_obj_set_flex_grow(sp, 1);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp, 0, 0);
    lv_obj_set_scrollbar_mode(sp, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sp, LV_OBJ_FLAG_CLICKABLE);

    return sp;
}

lv_obj_t *ui_memo_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    static const rt_uint32_t seg_rgb[MEMO_SEG_N] =
        { UI_MEMO_KNOWN, UI_MEMO_VAGUE, UI_MEMO_FORGOT };

    lv_obj_t *card = ui_card_create(parent, MEMO_CARD_W, MEMO_CARD_H);
    lv_obj_t *title, *note, *bar;
    int i;

    lv_obj_set_pos(card, x, y);

    /* 纵向 flex：字号升级（lv_conf 开 32 号）时自动重排 */
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, MEMO_PAD, 0);
    lv_obj_set_style_pad_row(card, 10, 0);

    title = ui_label(card, &ui_style_small_dim);
    lv_label_set_text(title, "Maimemo");

    memo_value = ui_label(card, &ui_style_value);
    lv_label_set_text(memo_value, "--");

    note = ui_label(card, &ui_style_small_dim);
    lv_label_set_text(note, "syncing");
    memo_note = note;

    flex_spacer(card);

    /* 三色条：UI_SEP 空轨道 + 三段比例填充（条上无文字，无图例） */
    bar = lv_obj_create(card);
    lv_obj_set_size(bar, MEMO_BAR_W, MEMO_BAR_H);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_SEP), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_scrollbar_mode(bar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    for (i = 0; i < MEMO_SEG_N; i++)
    {
        memo_seg[i] = lv_obj_create(bar);
        lv_obj_set_size(memo_seg[i], 0, MEMO_BAR_H);
        lv_obj_set_style_bg_color(memo_seg[i], lv_color_hex(seg_rgb[i]), 0);
        lv_obj_set_style_bg_opa(memo_seg[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(memo_seg[i], 0, 0);
        lv_obj_set_style_radius(memo_seg[i], 0, 0);
        lv_obj_add_flag(memo_seg[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(memo_seg[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(memo_seg[i], LV_OBJ_FLAG_CLICKABLE);
    }

    lv_timer_create(memo_timer_cb, 1000, NULL);
    memo_timer_cb(NULL);

    return card;
}
