/*
 * ui_theme.c - 主题公共实现：共享静态样式 + 直角卡片/标签工厂
 *
 * 共享样式让页面代码只表达"这块是什么角色"，具体字号/颜色集中在此，
 * 减少逐控件 inline 属性，缩短样式链。
 */
#include <rtthread.h>
#include <stdarg.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "cjk_font.h"

lv_style_t ui_style_text;          /* TEXT 字号 + 墨黑 */
lv_style_t ui_style_dim;           /* TEXT 字号 + 暖灰 */
lv_style_t ui_style_small;         /* SMALL 字号 + 墨黑 */
lv_style_t ui_style_small_dim;     /* SMALL 字号 + 暖灰 */
lv_style_t ui_style_value;         /* VALUE 字号 + 墨黑（大读数） */

static lv_style_t style_card;      /* 白板：白底 + 1px 描边 + 直角 */
static lv_style_t style_dark;      /* 墨黑色块：无描边 */

/* ---- 运行时字体选择 ----
 *
 * 字库从 SD 卡加载（cjk_font.c）。加载成功则使用它，因其含完整 GB2312，
 * 中文文案可正常显示；失败则退回编译期的 Montserrat，
 * 界面变为"英文可见、中文空白"，但仍可使用。
 *
 * 此处返回「加载结果」而非「当前字体」，目的是让调用点无需关心加载时序：
 * ui_theme_init() 先将样式指向内置字体，字库加载完成后再调用
 * ui_theme_apply_cjk_font() 把样式切换过去。 */
const lv_font_t *ui_font_text(void)
{
    lv_font_t *f = cjk_font_get();

    /* CJK 字库只含汉字，英文通过其 fallback 链（montserrat_16）渲染。
     * 见 cjk_font.c 中设置 fallback 的位置。 */
    return (f != RT_NULL) ? f : (&lv_font_montserrat_16);
}

const lv_font_t *ui_font_small(void)
{
    /* 小号字体始终使用 Montserrat 14，不切换到 CJK 字库。
     *
     * 理由：ui_style_small 的 19 个使用点全部为英文或数字，包括日历格、
     * 状态栏图标与 RSSI、抽屉中的应用名与状态字。这些位置没有中文，
     * 而 CJK 字库为 16px，放入 14px 的位置会使字符变大、行高变化，
     * 打乱已排好的布局。
     *
     * 唯一会显示中文的小号场景是便签正文，该处使用 ui_style_text
     * （见 ui_alarm.c 的 note 行），不受此处影响。 */
    return &lv_font_montserrat_14;
}

/* 字库加载完成后调用，把已有样式切到新字体。
 * 只改字体不改颜色：颜色语义与字体无关。 */
void ui_theme_apply_cjk_font(void)
{
    lv_font_t *f = cjk_font_get();

    if (f == RT_NULL)
        return;

    /* 只切换 TEXT 与 DIM 两个样式。
     *
     * 有意不修改 ui_style_small 与 ui_style_small_dim：它们的 19 个
     * 使用点全部为英文和数字（日历格、状态栏 RSSI、抽屉应用名），而 CJK
     * 字库为 16px，放入 14px 的位置会打乱布局。ui_font_small()
     * 也相应固定返回 Montserrat 14，两处保持一致。
     *
     * 真正需要中文的小号场景只有便签正文，它使用 ui_style_text。 */
    lv_style_set_text_font(&ui_style_text, f);
    lv_style_set_text_font(&ui_style_dim, f);

    /* 关键：仅修改样式对象不够，已存在的控件不会自动重新获取字体。
     *
     * LVGL 控件在创建时将 style 指针挂入自身的 styles 链表，绘制时读取的
     * 是 style 结构体中的值，因此理论上修改结构体即可生效。但字体的
     * glyph 缓存、行高与行距已被控件缓存，且布局（
     * LV_STYLE_PROP_FLAG_LAYOUT_UPDATE）需要重算。
     *
     * lv_obj_report_style_change() 会遍历所有屏幕树，将使用该样式的控件
     * 逐个 refresh（重算布局并标脏）。这是 LVGL 为"样式运行时变更"
     * 提供的标准入口（见 lv_obj_style.c:261）。
     *
     * 缺少该调用时，表现为"新建的控件能显示中文，但切换字体前已创建的
     * 控件仍为旧字体"，真机现象与此一致。 */
    lv_obj_report_style_change(&ui_style_text);
    lv_obj_report_style_change(&ui_style_dim);

    lv_obj_invalidate(lv_screen_active());

    rt_kprintf("[ui.theme] CJK font applied (text/dim only)\n");
}

void ui_theme_init(void)
{
    lv_style_init(&ui_style_text);
    lv_style_set_text_font(&ui_style_text, UI_FONT_TEXT);
    lv_style_set_text_color(&ui_style_text, lv_color_hex(UI_INK));

    lv_style_init(&ui_style_dim);
    lv_style_set_text_font(&ui_style_dim, UI_FONT_TEXT);
    lv_style_set_text_color(&ui_style_dim, lv_color_hex(UI_TEXT_DIM));

    lv_style_init(&ui_style_small);
    lv_style_set_text_font(&ui_style_small, UI_FONT_SMALL);
    lv_style_set_text_color(&ui_style_small, lv_color_hex(UI_INK));

    lv_style_init(&ui_style_small_dim);
    lv_style_set_text_font(&ui_style_small_dim, UI_FONT_SMALL);
    lv_style_set_text_color(&ui_style_small_dim, lv_color_hex(UI_TEXT_DIM));

    lv_style_init(&ui_style_value);
    lv_style_set_text_font(&ui_style_value, UI_FONT_VALUE);
    lv_style_set_text_color(&ui_style_value, lv_color_hex(UI_INK));

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, lv_color_hex(UI_CARD_BG));
    lv_style_set_border_color(&style_card, lv_color_hex(UI_CARD_EDGE));
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_radius(&style_card, 0);
    lv_style_set_pad_all(&style_card, 0);

    lv_style_init(&style_dark);
    lv_style_set_bg_color(&style_dark, lv_color_hex(UI_INK));
    lv_style_set_border_width(&style_dark, 0);
    lv_style_set_radius(&style_dark, 0);
    lv_style_set_pad_all(&style_dark, 0);
}

lv_obj_t *ui_label(lv_obj_t *parent, lv_style_t *st)
{
    lv_obj_t *label = lv_label_create(parent);

    lv_obj_add_style(label, st, 0);
    return label;
}

/* 手势/点击一律透给父级页面容器，整页滑动才不受阻 */
static void make_passthrough(lv_obj_t *obj)
{
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
}

lv_obj_t *ui_card_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_set_size(card, w, h);
    lv_obj_add_style(card, &style_card, 0);
    make_passthrough(card);

    return card;
}

lv_obj_t *ui_card_dark_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);

    lv_obj_set_size(card, w, h);
    lv_obj_add_style(card, &style_dark, 0);
    make_passthrough(card);

    return card;
}

bool ui_label_set_text_if_diff(lv_obj_t *lbl, const char *text)
{
    const char *cur = lv_label_get_text(lbl);

    if (cur != NULL && text != NULL && rt_strcmp(cur, text) == 0)
        return false;

    lv_label_set_text(lbl, text);
    return true;
}

bool ui_label_set_text_fmt_if_diff(lv_obj_t *lbl, const char *fmt, ...)
{
    char buf[128];
    va_list ap;

    va_start(ap, fmt);
    rt_vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    return ui_label_set_text_if_diff(lbl, buf);
}

bool ui_set_text_color_if_diff(lv_obj_t *obj, uint32_t rgb)
{
    lv_color_t want = lv_color_hex(rgb);

    if (lv_color_eq(lv_obj_get_style_text_color(obj, LV_PART_MAIN), want))
        return false;

    lv_obj_set_style_text_color(obj, want, 0);
    return true;
}

bool ui_set_bg_color_if_diff(lv_obj_t *obj, uint32_t rgb)
{
    lv_color_t want = lv_color_hex(rgb);

    if (lv_color_eq(lv_obj_get_style_bg_color(obj, LV_PART_MAIN), want))
        return false;

    lv_obj_set_style_bg_color(obj, want, 0);
    return true;
}
