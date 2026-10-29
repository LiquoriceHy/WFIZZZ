/*
 * ui_app_icons.c - 抽屉卡片的图标绘制
 *
 * 使用图元而非位图：
 *   40x40 的五个图标若做成 PNG 或 C 数组需占用 flash，每改一版都要重新
 *   生成资源并重新烧录，还会将 image 解码引入绘制热路径。此处全部用
 *   lv_obj 矩形与 lv_line 折线拼成（VG-Lite 对 fill 与 line 均有原生
 *   实现），零资源、跟随主题色、分辨率无关。
 *
 * 坐标约定：
 *   所有函数都在一个 40x40 的墨黑容器内绘制，坐标系即 0..40 的整数网格，
 *   不依赖父容器尺寸。图案占中间 24x24（四周留 8px 视觉留白），线宽 2px，
 *   方头端点（line_rounded = 0），与主题"全直角、无圆角"一致。
 *
 * lv_line 的语义：它把点集视为一条连续折线，相邻点之间全部连线。
 * 因此互不相连的线段必须各自成为一个 lv_line 对象，不能放入同一数组
 * （否则会多画出一条无意义的连接线）。下面的 icon_seg() 即为此类
 * 单段线准备。
 *
 * 颜色：
 *   前景色由调用方给出（抽屉中为卡片白）。图标只绘制前景图元，墨黑底由
 *   调用方创建的容器提供，因此"反色/高亮"等变化无需修改此处。
 *
 * 性能：
 *   这些控件仅在抽屉初始化时创建一次（build_app_cards 内），之后不再改动，
 *   不位于任何刷新路径上。抽屉每 300ms 仅修改文字，不触碰此处图形。
 */
#include <rtthread.h>
#include "lvgl.h"
#include "ui_app_icons.h"
#include "ui_theme.h"

/* 图标容器边长（与抽屉里的图标块一致） */
#define ICON_BOX        40

/* 线宽：2px 在 40px 方块里足够看清，又不会把 24px 的图形糊住 */
#define ICON_LINE_W     2

/* 一条连续折线。点集必须与被画次数同寿（static const 或调用方的长期数组）。 */
static lv_obj_t *icon_poly(lv_obj_t *parent, const lv_point_precise_t *pts,
                           uint32_t num, uint32_t color)
{
    lv_obj_t *line = lv_line_create(parent);

    lv_line_set_points(line, pts, num);
    lv_obj_set_style_line_width(line, ICON_LINE_W, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(color), 0);
    lv_obj_set_style_line_opa(line, LV_OPA_COVER, 0);
    /* 端点必须为圆头。
     *
     * lv_line 将相邻点逐段绘制为独立的线（见 lv_line.c 的 DRAW_MAIN：
     * 每段一次 lv_draw_line），并非一条 path。方头端点时两段在拐角处
     * 各自绘制，外侧会留下一个 1x1 的缺口，在白线黑底上表现为
     * "拐角缺角"。圆头端点的半圆帽正好填补该缝隙，而 1px 半径的圆角
     * 在 40px 图标中不可见，不影响直角观感。 */
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /* lv_line 默认按点集自算尺寸；这里给满容器、坐标用绝对值 */
    lv_obj_set_pos(line, 0, 0);
    lv_obj_set_size(line, ICON_BOX, ICON_BOX);

    return line;
}

/* 单段直线的点集池。
 *
 * lv_line 不复制点集，仅保存指针，因此这些数组必须与控件同生命周期：
 * 不能使用函数内的局部数组（函数返回后即失效），也不能共用一个会被
 * 复用的小池（写入第二段时会修改第一段仍在引用的坐标，表现为图标
 * 自身变形）。
 *
 * 实现：一个固定容量的静态表，每个 lv_line 独占一项，取用后不回收。
 * 每个图标最多 8 段，五个图标合计不足 16 段，容量取 24 以留余量。
 * 只在 build_app_cards() 里跑一次，容量是编译期已知的上界。 */
#define ICON_SEG_POOL   24

static lv_point_precise_t s_seg_pts[ICON_SEG_POOL][2];
static rt_uint32_t        s_seg_used;

static lv_obj_t *icon_seg(lv_obj_t *parent, lv_coord_t x1, lv_coord_t y1,
                          lv_coord_t x2, lv_coord_t y2, uint32_t color)
{
    lv_point_precise_t *pts;

    if (s_seg_used >= ICON_SEG_POOL)
    {
        /* 池满了说明图标段数超了预算。宁可不画这一段，也不要覆盖
         * 前面已经建好的图标（那会把整个抽屉的图案改花）。 */
        rt_kprintf("[ui.icon] seg pool exhausted\n");
        return RT_NULL;
    }

    pts = s_seg_pts[s_seg_used++];
    pts[0].x = x1;  pts[0].y = y1;
    pts[1].x = x2;  pts[1].y = y2;

    return icon_poly(parent, pts, 2, color);
}

/* 段池复位。抽屉只建一次卡片，这里主要是让"重建抽屉"时不会耗尽池。 */
void ui_app_icon_reset(void)
{
    s_seg_used = 0;
}

/* 实心方块 */
static lv_obj_t *icon_rect(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_coord_t h, uint32_t color)
{
    lv_obj_t *r = lv_obj_create(parent);

    lv_obj_set_size(r, w, h);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_style_bg_color(r, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);

    return r;
}

/* ---------------- Snore：字母 Z + 两道声波 ---------------- */

void ui_icon_snore(lv_obj_t *icon, uint32_t color)
{
    /* Z 是一条连续折线：上横 -> 斜线 -> 下横。三个顶点就够。 */
    static const lv_point_precise_t z_pts[] = {
        { 7, 9 }, { 16, 9 }, { 7, 21 }, { 16, 21 },
    };
    /* 声波：两道折线，左窄右宽，暗示向外扩散 */
    static const lv_point_precise_t w1_pts[] = {
        { 20, 13 }, { 22, 15 }, { 20, 17 },
    };
    static const lv_point_precise_t w2_pts[] = {
        { 25, 10 }, { 28, 15 }, { 25, 20 },
    };

    icon_poly(icon, z_pts, 4, color);
    icon_poly(icon, w1_pts, 3, color);
    icon_poly(icon, w2_pts, 3, color);
}

/* ---------------- Audio：电平竖条 ---------------- */

void ui_icon_audio(lv_obj_t *icon, uint32_t color)
{
    /* 五根竖条，中间最高、两侧递减，底边对齐于 y=29，即电平条。
     * 竖条使用实心矩形而非 lv_line：2px 宽的短线走 line 会出现端点
     * 抗锯齿，实心块边界更干净。 */
    static const struct { lv_coord_t x, y, h; } bars[] = {
        { 7,  19, 10 },
        { 12, 14, 15 },
        { 17, 8,  21 },
        { 22, 14, 15 },
        { 27, 19, 10 },
    };
    rt_uint32_t i;

    for (i = 0; i < sizeof(bars) / sizeof(bars[0]); i++)
        icon_rect(icon, bars[i].x, bars[i].y, ICON_LINE_W, bars[i].h, color);
}

/* ---------------- AI Chat：方角对话气泡 + 三点 ---------------- */

void ui_icon_chat(lv_obj_t *icon, uint32_t color)
{
    /* 气泡轮廓是一条闭合折线：矩形三边 + 左下角尾巴。
     * 尾巴和轮廓必须连成一体（否则会看到接缝），所以这里必须用
     * 单个 lv_line 而不是几段拼。 */
    static const lv_point_precise_t bubble_pts[] = {
        { 8, 9 }, { 32, 9 }, { 32, 25 }, { 17, 25 },
        { 12, 30 }, { 12, 25 }, { 8, 25 }, { 8, 9 },
    };
    rt_uint32_t i;

    icon_poly(icon, bubble_pts, 8, color);

    /* 三个点数：表示 AI 正在输出内容 */
    for (i = 0; i < 3; i++)
        icon_rect(icon, (lv_coord_t)(15 + i * 5), 16, 2, 2, color);
}

/* ---------------- Model Info：NPU 芯片 ---------------- */

void ui_icon_model(lv_obj_t *icon, uint32_t color)
{
    /* 芯片本体：闭合折线画四条边 */
    static const lv_point_precise_t body_pts[] = {
        { 12, 12 }, { 28, 12 }, { 28, 28 }, { 12, 28 }, { 12, 12 },
    };

    icon_poly(icon, body_pts, 5, color);

    /* 八根引脚：上下各两根竖线，左右各两根横线。
     * 每根都是独立的段，因此逐个调用 icon_seg；若放入同一
     * 点集，lv_line 会将它们连成一圈错误的线。 */
    icon_seg(icon, 17, 6,  17, 12, color);
    icon_seg(icon, 23, 6,  23, 12, color);
    icon_seg(icon, 17, 28, 17, 34, color);
    icon_seg(icon, 23, 28, 23, 34, color);
    icon_seg(icon, 6,  17, 12, 17, color);
    icon_seg(icon, 6,  23, 12, 23, color);
    icon_seg(icon, 28, 17, 34, 17, color);
    icon_seg(icon, 28, 23, 34, 23, color);

    /* 中心的算力核心：实心小块，和周围空心引脚形成对比 */
    icon_rect(icon, 18, 18, 4, 4, color);
}

/* ---------------- LAN Devices：三个节点 + 连线 ---------------- */

void ui_icon_lan(lv_obj_t *icon, uint32_t color)
{
    /* 一个父节点在上、两个子节点在下，连线组成倒 V。
     * 对应实际拓扑：M55 主机 -> 两台 MQTT 设备。
     * 两条边各自成段（不是一条折线，避免中间多画一次回头）。 */
    icon_seg(icon, 20, 12, 11, 26, color);
    icon_seg(icon, 20, 12, 29, 26, color);

    /* 三个节点：方角实心块 */
    icon_rect(icon, 17, 8,  6, 6, color);   /* 主机 */
    icon_rect(icon, 8,  26, 6, 6, color);   /* 设备 A */
    icon_rect(icon, 26, 26, 6, 6, color);   /* 设备 B */
}

/* ---------------- 分发 ---------------- */

void ui_app_icon_draw(lv_obj_t *icon, app_icon_t id, uint32_t color)
{
    switch (id)
    {
    case APP_ICON_SNORE: ui_icon_snore(icon, color); break;
    case APP_ICON_AUDIO: ui_icon_audio(icon, color); break;
    case APP_ICON_CHAT:  ui_icon_chat(icon, color);  break;
    case APP_ICON_MODEL: ui_icon_model(icon, color); break;
    case APP_ICON_LAN:   ui_icon_lan(icon, color);   break;

    case APP_ICON_LETTER:
    default:
        break;      /* 交由调用方画首字母 */
    }
}
