/*
 * ui_theme.h - 智能终端 UI 主题（颜色/字体/尺寸常量 + 公共卡片/标签）
 *
 * 风格：MiMo 式暖白底 + 墨黑排版锚点 + 全直角（直角屏，且 VG-Lite 矩形
 * path 比圆角快）。无阴影、无渐变（RGB565 渐变会色带）。
 *
 * 颜色语义（改色只改这里，页面代码不出现字面色值）：
 *   墨黑 UI_INK      强调：正文、时钟色块、今天、当前页点
 *   暖灰 UI_OFFLINE  无数据/未接入的占位
 *   橙   UI_ATTENTION 仅用于"需要关注"（离线/异常），正常态一律墨黑
 * 于是健康状态下整屏近乎单色暖白+墨黑，颜色只在该被看见时出现。
 *
 * 字体自适应：lv_conf.h 当前只开了 montserrat 12/14/16/20/22/24。
 * 在 lv_conf.h 打开 LV_FONT_MONTSERRAT_32/_48（置 1）后，时钟/读数
 * 字号自动升级，无需改本处代码。
 */
#ifndef __UI_THEME_H__
#define __UI_THEME_H__

#include "lvgl.h"

/* 颜色（24 位 0xRRGGBB。lv_color_hex() 按 RRGGBB 解析，不可写 RGB565；
 * 括号内为 RGB565 近似值，来源 mimo.mi.com 设计令牌） */
#define UI_BG          0xFCFAF8   /* 页面底色 #fcfaf8 暖白 (0xFFDF) */
#define UI_BAR_BG      UI_BG      /* 状态栏与页面融为一体 */
#define UI_CARD_BG     0xFFFFFF   /* 卡片/白板底色纯白 */
#define UI_CARD_EDGE   0xE5E5E5   /* 卡片描边 (0xE73C) */
#define UI_SEP         0xE5E5E5   /* 分隔线 (0xE73C) */
#define UI_INK         0x171717   /* 墨黑：正文与强调锚点 (0x10A2) */
#define UI_TEXT        UI_INK     /* 正文同墨黑 */
#define UI_TEXT_DIM    0x74716D   /* 次要文字（暖灰）(0x738D) */
#define UI_MUTED       0xC9C4BD   /* 非活跃指示（页点等）(0xCE38) */
#define UI_ATTENTION   0xFDA83A   /* 橙：需要关注（离线/异常）(0xFD47) */
#define UI_OFFLINE     0x8C8880   /* 无数据/未接入占位 (0x8C50) */

/* 图表（Snore 应用页的时间轴）。
 * 这是继墨墨三色条之后第二个允许出现的彩色区域，理由相同：
 * 它是"数据本身的形状"，不是装饰。 */
#define UI_CHART_BAR   0x5B7CFA   /* 事件标记蓝 */
#define UI_CHART_TRACK 0xF5F5F5   /* 轴底浅灰：比卡片白略深，让标记有底 */

/* 墨墨三色条：官方色（认识 #36B59D / 模糊 #EB9E27 / 忘记 #DC663E）降饱和，
 * 贴合暖白主题；是卡片内唯一的彩色元素 */
#define UI_MEMO_KNOWN  0x7FC5B4   /* 认识 柔青绿 */
#define UI_MEMO_VAGUE  0xE8C58A   /* 模糊 柔琥珀 */
#define UI_MEMO_FORGOT 0xD99C86   /* 忘记 柔陶红 */

/* 字体（未开启的字号自动回退） */
#if LV_FONT_MONTSERRAT_48
#define UI_FONT_CLOCK   (&lv_font_montserrat_48)
#else
#define UI_FONT_CLOCK   (&lv_font_montserrat_24)
#endif

#if LV_FONT_MONTSERRAT_32
#define UI_FONT_VALUE   (&lv_font_montserrat_32)
#else
#define UI_FONT_VALUE   (&lv_font_montserrat_22)
#endif

/* TEXT / SMALL 为运行时字体：字库从 SD 卡加载（见 cjk_font.h），
 * 加载成功则使用该字库，否则退回编译期的 Montserrat（纯 ASCII）。
 *
 * 未实现为宏的原因：字库在开机后才确定，宏在编译期即固化。
 * 此处使用函数而非宏，调用点写法不变（UI_FONT_TEXT 仍是可当作
 * lv_font_t* 使用的表达式），仅多一次函数调用；该函数只读一个静态
 * 指针，开销可忽略，且不位于绘制热路径（样式仅在创建控件时设置）。
 *
 * 注意：不能用于静态初始化器（C 语言不允许在 static initializer 中
 * 出现函数调用）。本工程的样式均在 ui_theme_init() 中于运行时设置，
 * 不存在该问题。 */
const lv_font_t *ui_font_text(void);
const lv_font_t *ui_font_small(void);

#define UI_FONT_TEXT    ui_font_text()
#define UI_FONT_SMALL   ui_font_small()

/* 尺寸 */
#define UI_SCREEN_W     800
#define UI_SCREEN_H     480
#define UI_BAR_H        36
#define UI_PAGE_H       (UI_SCREEN_H - UI_BAR_H)   /* 444 */
#define UI_PAD          24      /* 页面外边距 */

/* 共享静态样式（ui_theme.c 中初始化，ui_theme_init() 必须在任何
 * 页面创建前调用一次；之后用 ui_label()/ui_card_create() 复用，
 * 避免逐控件 inline 样式，缩短样式链、减少重绘属性） */
extern lv_style_t ui_style_text;          /* TEXT 字号 + 墨黑 */
extern lv_style_t ui_style_dim;           /* TEXT 字号 + 暖灰 */
extern lv_style_t ui_style_small;         /* SMALL 字号 + 墨黑 */
extern lv_style_t ui_style_small_dim;     /* SMALL 字号 + 暖灰 */
extern lv_style_t ui_style_value;         /* VALUE 字号 + 墨黑（大读数） */

void ui_theme_init(void);

/* 中文字库加载完成后调用：把共享样式的字体切到运行时字库并重绘整屏。
 * 字库加载失败时是空操作，界面继续用内置字库。 */
void ui_theme_apply_cjk_font(void);

/* 标签工厂：套用共享样式（字号/颜色） */
lv_obj_t *ui_label(lv_obj_t *parent, lv_style_t *st);

/* 白板：直角矩形 + 1px 描边，各页面复用（实现在 ui_theme.c） */
lv_obj_t *ui_card_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);
/* 墨黑色块：页面唯一的大色块锚点，无描边 */
lv_obj_t *ui_card_dark_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h);

/* 值变化才写入（1Hz 刷新守卫）：LVGL 的 set_text 与 set_style 无同值短路，
 * 文本相同也会 invalidate 并 free/malloc，颜色相同也会双向 invalidate，
 * 而底层任何 dirty 都会触发全屏呈现（768KB cache clean 与 VG-Lite 全屏
 * 旋转）。周期定时器回调一律使用这组 helper，将"无变化"拦截在重绘之前。
 * 返回 true 表示值确实改变（已写入）。 */
bool ui_label_set_text_if_diff(lv_obj_t *lbl, const char *text);
bool ui_label_set_text_fmt_if_diff(lv_obj_t *lbl, const char *fmt, ...);
bool ui_set_text_color_if_diff(lv_obj_t *obj, uint32_t rgb);
bool ui_set_bg_color_if_diff(lv_obj_t *obj, uint32_t rgb);

#endif
