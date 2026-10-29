/*
 * app_registry.c - 应用表 + mic0 互斥
 *
 * 新增应用只改本文件的 s_apps[]：写一个 get_status（可选）与 start/stop
 * （可选），然后加一行描述符。UI 完全不用改。
 */
#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "app_registry.h"
#include "ui_theme.h"
#include "ui_lanctl.h"
#include "snore_rt.h"
#include "snore_sum.h"      /* 归档视图：最近 3 天的汇总快照（后台线程读盘） */
#include "xz_app.h"
#include "app_data.h"
#include "snore_infer.h"
#include "audio_event.h"
#include "lanctl.h"
#include "screen_saver.h"   /* 聊天页收到新消息时续期，避免看对话时息屏 */

#define DBG_TAG "app.reg"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/*
 * RMS 电平转换为 0-100 的显示量。
 *
 * 采用对数的原因：人耳与麦克风电平均为对数感知。线性映射时安静环境
 * （RMS 20-80）几乎贴近 0，说话或鼾声（RMS 800-3000）也仅到 10% 左右，
 * 进度条无法体现变化。取 dB 再归一化后，安静与有声的对比才明显。
 *
 * 映射：RMS 32 对应 0%，RMS 8192 对应 100%（约 48dB 动态范围）。
 */
static rt_uint8_t rms_to_level(rt_uint32_t rms)
{
    float db, pct;

    if (rms < 32)
        return 0;

    /* 20*log10(rms/32) / 48dB * 100 */
    db = 20.0f * (float)log10((double)rms / 32.0);
    pct = db * 100.0f / 48.0f;

    if (pct < 0.0f)
        pct = 0.0f;
    if (pct > 100.0f)
        pct = 100.0f;

    return (rt_uint8_t)(pct + 0.5f);
}

/* snore_server.c 提供（没有独立头文件，这里前置声明） */
extern rt_bool_t snore_server_is_infer_ready(void);
extern rt_bool_t snore_server_is_running(void);
extern void      snore_server_get_arena_text(char *buf, int len);

/* ---------------- 应用 1：鼾声实时检测 ---------------- */

static rt_bool_t snore_is_running(void)
{
    snore_rt_status_t st;

    snore_rt_get_status(&st);
    return st.running;
}

static rt_uint8_t snore_get_level(void)
{
    snore_rt_status_t st;

    snore_rt_get_status(&st);
    if (!st.running)
        return 0;

    return rms_to_level(st.rms);
}

static void snore_get_status(char *buf, int len)
{
    snore_rt_status_t st;

    snore_rt_get_status(&st);

    if (!st.running)
    {
        rt_snprintf(buf, len, "Idle   th %u%%", (unsigned)st.threshold);
        return;
    }

    rt_snprintf(buf, len, "%s  prob %u%%  w%u e%u",
                st.mqtt_up ? "MQTT" : "no-net",
                (unsigned)st.last_prob,
                (unsigned)st.windows, (unsigned)st.events);
}

static void snore_get_detail(char *buf, int len)
{
    snore_rt_status_t st;
    snore_rt_hist_t   hist[5];
    int n, i, pos;

    snore_rt_get_status(&st);

    pos = rt_snprintf(buf, len,
                      "%s  MQTT %s  th %u%%  w%u e%u  prob %u%%\n"
                      "--- events (newest first) ---\n",
                      st.running ? "Running" : "Stopped",
                      st.mqtt_up ? "on" : "off",
                      (unsigned)st.threshold,
                      (unsigned)st.windows, (unsigned)st.events,
                      (unsigned)st.last_prob);

    n = snore_rt_get_history(hist, 5);
    if (n <= 0)
    {
        if (pos < len)
            rt_snprintf(buf + pos, len - pos, "  (none yet)");
        return;
    }

    for (i = 0; i < n && pos < len; i++)
    {
        rt_uint32_t s = hist[i].uptime_s;

        pos += rt_snprintf(buf + pos, len - pos,
                           "  +%u:%02u:%02u  prob %u%%  %ums\n",
                           (unsigned)(s / 3600u),
                           (unsigned)((s / 60u) % 60u),
                           (unsigned)(s % 60u),
                           (unsigned)hist[i].prob,
                           (unsigned)hist[i].cost_ms);
    }
}

/* ---------------- 应用屏公共小件 + Snore 应用屏幕 ----------------
 *
 * 概览小方块（app_tile）是 Snore / Audio 详情页共用的骨架件。
 *
 * Snore 布局（2026-10-28：最近 3 天、按天分页、左图表 + 右汇总）：
 *   概览行（4 个小方块：MQTT / 阈值 / 窗数 / 最近概率），全宽
 *   翻页栏（<  2026-10-28  TODAY  TOTAL 128  >），全宽
 *   左半（354px）：时间轴图表 —— 横轴从当天第一次事件到最后一次，
 *                 有事件的时段画蓝色竖条
 *   右半（386px）：汇总卡 —— total / avg+max / heavy / peak / last
 *
 * 为什么左图表右汇总而不是列表：一天的记录量级 500-2000 条，列表只能
 * 显示最近几条且看不出"分布"；时间轴一眼就能看出打鼾集中在几点。
 *
 * 数据来自 snore_sum 后台线程的内存快照，**不在本函数里读 SD**：
 * draw 每 500ms 跑一次且位于 LVGL 线程（栈 24KB、1ms 刷新周期），一次
 * 扫描要读完整个 CSV（单日上限 5760 行），直接读会造成持续的 UI 冻结。
 *
 * 时钟未同步（NTP 未成功）时回退到"本次开机"视图：读 snore_rt 的内存
 * 环形缓冲。此时按天分页与时间轴都没有意义——软 RTC 从 2018 起算，
 * 算出的"今天"是假日期，读到的也不是用户今天的数据。
 *
 * draw 每 500ms 被调用一次，必须保证重复调用安全：控件仅在第一次创建
 * （判断 *_val[0] == NULL），之后只修改文本，且一律经过 ui_theme 的
 * if_diff 守卫，值未变化时不 invalidate。static 指针在 undraw 中清零
 * （控件本体由 ui_app 统一 lv_obj_clean 删除，指针不能保留以免解引用）。
 */

#define APP_TILE_W      176     /* 概览方块宽（Snore/Audio 共用） */
#define APP_TILE_H      84
#define SN_CONTENT_W    (APP_TILE_W * 4 + 12 * 3)   /* 740 */

/* 翻页栏：740 x 32。左右箭头各 44 宽，中间信息左对齐。
 * 箭头用 ASCII "<" / ">" 而非 LV_SYMBOL_LEFT/RIGHT：后者需要 FontAwesome
 * 段，虽可用但与全页 ASCII 风格不一致（ui_system.c 有同样的取舍记录）。 */
#define SN_NAV_H        32
#define SN_NAV_ARROW_W  44
#define SN_NAV_TOP      (APP_TILE_H + 20)           /* = 104 */

/* 内容区（图表 + 汇总）的纵向起点与左右分栏
 *   总宽 740，中间留 12 间隙 → 左 354、右 374
 *
 * 左列纵向预算（从 SN_MAIN_TOP=152 起，内容区到 374）：
 *   标题 14 / 轴底 72 / 基线 8 / 刻度 18 / 图例 20 = 222，末端 374。
 *   刚好用满，不留多余空白。 */
#define SN_MAIN_TOP     (SN_NAV_TOP + SN_NAV_H + 16)   /* = 152 */
#define SN_GUTTER       12
#define SN_LEFT_W       354
#define SN_RIGHT_X      (SN_LEFT_W + SN_GUTTER)        /* = 366 */
#define SN_RIGHT_W      (SN_CONTENT_W - SN_RIGHT_X)    /* = 374 */

#define SN_TITLE_H      18
#define SN_PLOT_TOP     (SN_MAIN_TOP + SN_TITLE_H)     /* = 170 */
#define SN_PLOT_H       72
#define SN_AXIS_TOP     (SN_PLOT_TOP + SN_PLOT_H)      /* = 242 */
#define SN_TICK_LBL_TOP (SN_AXIS_TOP + 6)              /* = 248 */
#define SN_LEGEND_TOP   (SN_TICK_LBL_TOP + 22)         /* = 270 */

/* 时间轴标记：轨道 354px，分 70 档，每档 5px（最后一档 4px）。
 * 与 snore_log.h 的 SNORE_LOG_AXIS_BUCKETS 必须一致。 */
#define SN_BUCKET_PX    5
#define SN_MARK_W       5

/* 标记矩形池大小。
 *
 * 为什么不给 70 个档位各建一个对象：每个 lv_obj 约 200-250 字节，
 * 70 个要 15KB 以上，而主堆余量只有约 40KB——为一个图表吃掉三分之一
 * 不合适。改成把相邻的已置位档合并成"段"，用 16 个矩形画；段数超过 16
 * 时合并间隙最小的两段（降精度但不丢"哪里有事件"的信息）。
 *
 * 正常睡眠数据只有几段，池子大部分时间是空的。 */
#define SN_MARK_POOL    16

/* 汇总卡 5 行的纵向位置（行高 28，卡高 SN_RIGHT_H） */
#define SN_SUM_LINE_H   28
#define SN_SUM_PAD_Y    12
#define SN_RIGHT_H      (SN_SUM_PAD_Y * 2 + SN_SUM_LINE_H * 5)  /* = 164 */

/* 开机视图（无日历时间时）的事件卡片 —— 与归档视图互斥使用 */
#define SN_BOOT_ROWS    6
#define SN_BOOT_CARD_H  30
#define SN_BOOT_CARD_GAP 4

static lv_obj_t *sn_ovw_val[4];      /* MQTT/阈值/窗数/概率 的数值标签 */

/* 翻页栏 */
static lv_obj_t *sn_nav_prev;        /* "<" 按钮 */
static lv_obj_t *sn_nav_next;        /* ">" 按钮 */
static lv_obj_t *sn_nav_lbl;         /* 中间信息：日期 + TODAY + 总数 */

/* 左列：时间轴图表 */
static lv_obj_t *sn_chart_title;     /* "SNORE" */
static lv_obj_t *sn_plot_bg;         /* 轴底浅灰矩形 */
static lv_obj_t *sn_mark[SN_MARK_POOL];   /* 蓝色标记矩形池 */
static int       sn_mark_used;       /* 本帧用到的标记数 */
static lv_obj_t *sn_axis_line;       /* 基线（1px 横线） */
static lv_obj_t *sn_axis_tick[3];    /* 中间 3 个刻度短竖线 */
static lv_obj_t *sn_tick_lbl[4];     /* 4 个时间刻度标签 */
static lv_obj_t *sn_legend_box;      /* 图例色块 */
static lv_obj_t *sn_legend_name;     /* 图例名称 */
static lv_obj_t *sn_legend_val;      /* 图例数值 */
static lv_obj_t *sn_chart_hint;      /* 无数据时的提示（"no events"/"clock not synced"） */

/* 右列：汇总卡 */
static lv_obj_t *sn_sum_card;
static lv_obj_t *sn_sum_val[5];

/* 上次绘制用的位图与区间：相同则跳过整段重绘。
 * draw 每 500ms 跑一次，不做这个守卫会让 16 个矩形持续标脏。 */
static rt_uint8_t sn_last_mask[SNORE_LOG_AXIS_BYTES];
static rt_uint32_t sn_last_first, sn_last_last;
static rt_bool_t  sn_last_valid;

/* 开机视图的事件卡片 */
static lv_obj_t *sn_row_card[SN_BOOT_ROWS];
static lv_obj_t *sn_row_time[SN_BOOT_ROWS];
static lv_obj_t *sn_row_prob[SN_BOOT_ROWS];
static lv_obj_t *sn_row_cost[SN_BOOT_ROWS];
static int       sn_rows_used;
static lv_obj_t *sn_empty_lbl;

/* 当前查看第几天：0 = 今天，1 = 昨天，2 = 前天。
 * 跨次进入应用页**故意保留**（与 ui_lanctl.c 的 lui_sel 同理）：
 * 用户翻到昨天看完退出，再进来还应该在昨天。
 * 这是唯一不在 undraw 里复位的 Snore 状态。 */
static int sn_day_idx = 0;

/* 箭头当前的禁用态缓存（-1 = 还没设置过）。
 * add/remove_state 无同值短路，每 500ms 无条件调用会持续重绘。 */
static rt_int8_t sn_nav_prev_off = -1;
static rt_int8_t sn_nav_next_off = -1;

/* 汇总卡左列文案 */
static const char *const sn_sum_key[5] =
{
    "total", "prob", "heavy", "peak", "last"
};

static void app_tile(lv_obj_t *parent, lv_coord_t x, const char *cap,
                     lv_obj_t **val_out)
{
    lv_obj_t *card = ui_card_create(parent, APP_TILE_W, APP_TILE_H);
    lv_obj_t *cap_l;
    lv_obj_t *val_l;

    lv_obj_set_pos(card, x, 0);

    cap_l = ui_label(card, &ui_style_small_dim);
    lv_label_set_text(cap_l, cap);
    lv_obj_set_pos(cap_l, 14, 12);

    val_l = ui_label(card, &ui_style_value);
    lv_label_set_text(val_l, "-");
    lv_obj_set_pos(val_l, 14, 34);

    *val_out = val_l;
}

/* 翻页回调：只改索引 + 请求重读，**不在这里删建控件**。
 *
 * 事件回调里 lv_obj_clean/delete 是不安全的（ui_alarm.c 有过同类教训），
 * 而且这里改了索引就要等后台线程重读完成才有新数据。让下一次 500ms 的
 * draw 统一处理，逻辑只有一条路径。 */
static void sn_nav_cb(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    int next  = sn_day_idx + delta;

    if (next < 0 || next >= SNORE_SUM_DAYS)
        return;

    sn_day_idx = next;
    snore_sum_request_refresh();
}

/* ===================== 左列：时间轴图表 ===================== */

/* 建图表骨架（只建一次） */
static void sn_chart_build(lv_obj_t *parent)
{
    int i;

    sn_chart_title = ui_label(parent, &ui_style_small_dim);
    lv_label_set_text(sn_chart_title, "SNORE");
    lv_obj_set_pos(sn_chart_title, 0, SN_MAIN_TOP);

    /* 轴底：浅灰底让蓝色标记有依托，也让"轨道范围"可见 */
    sn_plot_bg = lv_obj_create(parent);
    lv_obj_set_size(sn_plot_bg, SN_LEFT_W, SN_PLOT_H);
    lv_obj_set_pos(sn_plot_bg, 0, SN_PLOT_TOP);
    lv_obj_set_style_radius(sn_plot_bg, 0, 0);
    lv_obj_set_style_bg_color(sn_plot_bg, lv_color_hex(UI_CHART_TRACK), 0);
    lv_obj_set_style_border_width(sn_plot_bg, 0, 0);
    lv_obj_set_style_pad_all(sn_plot_bg, 0, 0);
    lv_obj_clear_flag(sn_plot_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sn_plot_bg, LV_OBJ_FLAG_CLICKABLE);

    /* 标记矩形池：先全建出来，之后只改位置/尺寸/可见性 */
    for (i = 0; i < SN_MARK_POOL; i++)
    {
        sn_mark[i] = lv_obj_create(parent);
        lv_obj_set_size(sn_mark[i], SN_MARK_W, SN_PLOT_H);
        lv_obj_set_style_radius(sn_mark[i], 0, 0);
        lv_obj_set_style_bg_color(sn_mark[i], lv_color_hex(UI_CHART_BAR), 0);
        lv_obj_set_style_border_width(sn_mark[i], 0, 0);
        lv_obj_set_style_pad_all(sn_mark[i], 0, 0);
        lv_obj_clear_flag(sn_mark[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(sn_mark[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(sn_mark[i], LV_OBJ_FLAG_HIDDEN);
    }
    sn_mark_used = 0;

    /* 基线：贯穿整个轨道 */
    sn_axis_line = lv_obj_create(parent);
    lv_obj_set_size(sn_axis_line, SN_LEFT_W, 1);
    lv_obj_set_pos(sn_axis_line, 0, SN_AXIS_TOP);
    lv_obj_set_style_radius(sn_axis_line, 0, 0);
    lv_obj_set_style_bg_color(sn_axis_line, lv_color_hex(UI_CARD_EDGE), 0);
    lv_obj_set_style_border_width(sn_axis_line, 0, 0);
    lv_obj_clear_flag(sn_axis_line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sn_axis_line, LV_OBJ_FLAG_CLICKABLE);

    /* 中间 3 个刻度（把轨道四等分） */
    for (i = 0; i < 3; i++)
    {
        sn_axis_tick[i] = lv_obj_create(parent);
        lv_obj_set_size(sn_axis_tick[i], 1, 5);
        lv_obj_set_pos(sn_axis_tick[i],
                       (lv_coord_t)(SN_LEFT_W * (i + 1) / 4), SN_AXIS_TOP);
        lv_obj_set_style_radius(sn_axis_tick[i], 0, 0);
        lv_obj_set_style_bg_color(sn_axis_tick[i], lv_color_hex(UI_CARD_EDGE), 0);
        lv_obj_set_style_border_width(sn_axis_tick[i], 0, 0);
        lv_obj_clear_flag(sn_axis_tick[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(sn_axis_tick[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* 4 个刻度标签：两端贴边（左端左对齐、右端右对齐），中间两个居中 */
    for (i = 0; i < 4; i++)
    {
        sn_tick_lbl[i] = ui_label(parent, &ui_style_small_dim);
        lv_label_set_text(sn_tick_lbl[i], "--:--");

        if (i == 0)
            lv_obj_set_pos(sn_tick_lbl[i], 0, SN_TICK_LBL_TOP);
        else if (i == 3)
            lv_obj_align(sn_tick_lbl[i], LV_ALIGN_TOP_RIGHT, -SN_GUTTER, SN_TICK_LBL_TOP);
        else
        {
            lv_coord_t x = (lv_coord_t)(SN_LEFT_W * i / 4);

            /* 5 字符的 "HH:MM" 在 14px 下约 38px 宽，居中要减去一半 */
            lv_obj_set_pos(sn_tick_lbl[i], x - 19, SN_TICK_LBL_TOP);
        }
    }

    /* 图例：色块 + 名称 + 数值 */
    sn_legend_box = lv_obj_create(parent);
    lv_obj_set_size(sn_legend_box, 12, 12);
    lv_obj_set_pos(sn_legend_box, 0, SN_LEGEND_TOP + 2);
    lv_obj_set_style_radius(sn_legend_box, 0, 0);
    lv_obj_set_style_bg_color(sn_legend_box, lv_color_hex(UI_CHART_BAR), 0);
    lv_obj_set_style_border_width(sn_legend_box, 0, 0);
    lv_obj_clear_flag(sn_legend_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(sn_legend_box, LV_OBJ_FLAG_CLICKABLE);

    sn_legend_name = ui_label(parent, &ui_style_small_dim);
    lv_label_set_text(sn_legend_name, "snore events");
    lv_obj_set_pos(sn_legend_name, 20, SN_LEGEND_TOP);

    sn_legend_val = ui_label(parent, &ui_style_small);
    lv_label_set_text(sn_legend_val, "-");
    lv_obj_set_pos(sn_legend_val, SN_LEFT_W - 90, SN_LEGEND_TOP);

    /* 无数据/时钟无效时的提示：叠在轨道中央 */
    sn_chart_hint = ui_label(parent, &ui_style_small_dim);
    lv_label_set_text(sn_chart_hint, "");
    lv_obj_set_pos(sn_chart_hint, 12, SN_PLOT_TOP + 26);
}

/* 把位图压成 ≤max_runs 个连续段。
 *
 * 段存成扁平数组：runs[2*i] = 起点，runs[2*i+1] = 终点（闭区间）。返回段数。
 *
 * 用一维而不是 int[][2]：二维数组的元素不能整体赋值（C 不允许
 * runs[i] = runs[j]），而合并逻辑必须搬移元素。
 *
 * scratch 至少能放 2*SNORE_LOG_AXIS_BUCKETS 个 int：**必须先把全部段
 * 提取出来**再压缩，不能边提取边限制数量——那样超出 max_runs 的段根本
 * 进不了数组，后面的"合并间隙最小"就无从下手，结果是图表右侧凭空空掉。
 *
 * 段数超过池大小时反复合并间隙最小的相邻两段：丢失的信息最少（只是把
 * 靠得很近的两段连起来），比直接丢弃某些段要诚实得多。 */
static int sn_axis_runs(const rt_uint8_t *mask, int *runs, int max_runs,
                        int *scratch)
{
    int n = 0;
    int b = 0;

    /* 提取全部连续段到 scratch */
    while (b < SNORE_LOG_AXIS_BUCKETS)
    {
        int s, e;

        if (!(mask[b >> 3] & (1u << (b & 7))))
        {
            b++;
            continue;
        }

        s = b;
        while (b < SNORE_LOG_AXIS_BUCKETS && (mask[b >> 3] & (1u << (b & 7))))
            b++;
        e = b - 1;

        scratch[2 * n]     = s;
        scratch[2 * n + 1] = e;
        n++;
    }

    /* 合并间隙最小的相邻段，直到塞得进池子。
     * 循环上界由 n > max_runs 保证，每轮必减 1。 */
    while (n > max_runs)
    {
        int best = 0;
        int best_gap = 0x7FFFFFFF;
        int i;

        for (i = 0; i + 1 < n; i++)
        {
            int gap = scratch[2 * (i + 1)] - scratch[2 * i + 1];

            if (gap < best_gap)
            {
                best_gap = gap;
                best = i;
            }
        }

        /* 把第 best+1 段的终点并进第 best 段，再整体左移一格 */
        scratch[2 * best + 1] = scratch[2 * (best + 1) + 1];
        for (i = best + 1; i + 1 < n; i++)
        {
            scratch[2 * i]     = scratch[2 * (i + 1)];
            scratch[2 * i + 1] = scratch[2 * (i + 1) + 1];
        }
        n--;
    }

    if (n > 0 && runs != RT_NULL)
        rt_memcpy(runs, scratch, (rt_size_t)n * 2 * sizeof(int));

    return n;
}

/* 把秒数格式化成 "HH:MM" */
static void sn_sod_hm(rt_uint32_t sod, char *out, rt_size_t len)
{
    rt_snprintf(out, len, "%02u:%02u",
                (unsigned)((sod % 86400) / 3600),
                (unsigned)(((sod % 86400) % 3600) / 60));
}

/* 按快照刷新左侧图表 */
static void sn_chart_update(const snore_sum_snapshot_t *snap)
{
    const snore_log_axis_t *ax = &snap->day[sn_day_idx].axis;
    rt_uint32_t total = snap->day[sn_day_idx].sum.total;
    char buf[48];

    /* 图例数值：事件条数 */
    if (total > 0)
        rt_snprintf(buf, sizeof(buf), "%u events", (unsigned)total);
    else
        rt_snprintf(buf, sizeof(buf), "0 events");
    ui_label_set_text_if_diff(sn_legend_val, buf);

    /* 零事件：清空标记与刻度，给出提示 */
    if (ax->total == 0)
    {
        int i;

        for (i = 0; i < SN_MARK_POOL; i++)
        {
            if (sn_mark[i] != RT_NULL)
                lv_obj_add_flag(sn_mark[i], LV_OBJ_FLAG_HIDDEN);
        }
        for (i = 0; i < 4; i++)
        {
            if (sn_tick_lbl[i] != RT_NULL)
                ui_label_set_text_if_diff(sn_tick_lbl[i], "");
        }
        ui_label_set_text_if_diff(sn_chart_hint, "no events this day");
        sn_last_valid = RT_FALSE;
        sn_mark_used = 0;
        return;
    }

    ui_label_set_text_if_diff(sn_chart_hint, "");

    /* 单个事件：轴上只有一个时刻，画一条居中的短线，4 个刻度同值。
     * 不能按"区间"处理——那会把一次事件画成横贯全轴的色块。 */
    if (ax->last_sod <= ax->first_sod)
    {
        char t[16];
        int i;

        sn_sod_hm(ax->first_sod, t, sizeof(t));

        for (i = 0; i < 4; i++)
            ui_label_set_text_if_diff(sn_tick_lbl[i], t);

        if (!sn_last_valid)
        {
            lv_coord_t x = SN_LEFT_W / 2 - 2;

            lv_obj_set_pos(sn_mark[0], x, SN_PLOT_TOP);
            lv_obj_set_size(sn_mark[0], 4, SN_PLOT_H);
            lv_obj_clear_flag(sn_mark[0], LV_OBJ_FLAG_HIDDEN);
            for (i = 1; i < SN_MARK_POOL; i++)
                lv_obj_add_flag(sn_mark[i], LV_OBJ_FLAG_HIDDEN);

            sn_last_valid = RT_TRUE;
            sn_last_first = ax->first_sod;
            sn_last_last  = ax->last_sod;
            rt_memset(sn_last_mask, 0, sizeof(sn_last_mask));
            sn_mark_used = 1;
        }
        return;
    }

    /* 刻度标签：左端 = 第一次，右端 = 最后一次，中间按时间线性插值 */
    {
        rt_uint32_t first = ax->first_sod;
        rt_uint32_t last  = ax->last_sod;
        int i;

        for (i = 0; i < 4; i++)
        {
            rt_uint32_t sod = first +
                (rt_uint32_t)((last - first) * (rt_uint64_t)i / 3);

            sn_sod_hm(sod, buf, sizeof(buf));
            ui_label_set_text_if_diff(sn_tick_lbl[i], buf);
        }
    }

    /* 位图与区间都没变：跳过全部标记重排（draw 每 500ms 一次，
     * 不做这个守卫会让 16 个矩形每帧都标脏） */
    if (sn_last_valid &&
        sn_last_first == ax->first_sod && sn_last_last == ax->last_sod &&
        rt_memcmp(sn_last_mask, ax->mask, sizeof(sn_last_mask)) == 0)
        return;

    sn_last_first = ax->first_sod;
    sn_last_last  = ax->last_sod;
    rt_memcpy(sn_last_mask, ax->mask, sizeof(sn_last_mask));
    sn_last_valid = RT_TRUE;

    /* 位图 → 连续段 → 矩形 */
    {
        int runs[SN_MARK_POOL * 2];
        int scratch[SNORE_LOG_AXIS_BUCKETS * 2];
        int n = sn_axis_runs(ax->mask, runs, SN_MARK_POOL, scratch);
        int i;

        for (i = 0; i < n; i++)
        {
            /* 档位换算回横坐标。span>0 由上面的判断保证。 */
            lv_coord_t x0 = (lv_coord_t)((rt_uint64_t)SN_LEFT_W * runs[2 * i]
                                         / SNORE_LOG_AXIS_BUCKETS);
            lv_coord_t x1 = (lv_coord_t)((rt_uint64_t)SN_LEFT_W * (runs[2 * i + 1] + 1)
                                         / SNORE_LOG_AXIS_BUCKETS);
            lv_coord_t w = x1 - x0;

            if (w < 2)
                w = 2;      /* 再窄也要看得见 */

            lv_obj_set_pos(sn_mark[i], x0, SN_PLOT_TOP);
            lv_obj_set_size(sn_mark[i], w, SN_PLOT_H);
            lv_obj_clear_flag(sn_mark[i], LV_OBJ_FLAG_HIDDEN);
        }

        /* 多余的隐藏 */
        for (i = n; i < SN_MARK_POOL; i++)
        {
            if (sn_mark[i] != RT_NULL)
                lv_obj_add_flag(sn_mark[i], LV_OBJ_FLAG_HIDDEN);
        }
        sn_mark_used = n;
    }
}

/* ===================== 右列：汇总卡 ===================== */

static void sn_sum_build(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card_create(parent, SN_RIGHT_W, SN_RIGHT_H);
    int i;

    lv_obj_set_pos(card, SN_RIGHT_X, SN_PLOT_TOP - SN_TITLE_H);

    for (i = 0; i < 5; i++)
    {
        lv_obj_t *k = ui_label(card, &ui_style_small_dim);
        lv_obj_t *v = ui_label(card, &ui_style_small);

        lv_label_set_text(k, sn_sum_key[i]);
        lv_obj_set_pos(k, 14, SN_SUM_PAD_Y + i * SN_SUM_LINE_H);

        lv_label_set_text(v, "-");
        lv_obj_set_pos(v, 90, SN_SUM_PAD_Y + i * SN_SUM_LINE_H);

        sn_sum_val[i] = v;
    }

    sn_sum_card = card;
}

static void sn_sum_update(lv_obj_t *parent, const snore_sum_snapshot_t *snap)
{
    const snore_log_summary_t *s = &snap->day[sn_day_idx].sum;
    char buf[64];

    RT_UNUSED(parent);

    /* 骨架由 snore_draw 首次进入时统一建好；这里若为空说明调用顺序错了，
     * 早退而不是懒建——懒建会让 parent 依赖变得不可推理。 */
    if (sn_sum_card == RT_NULL)
        return;

    /* total */
    ui_label_set_text_fmt_if_diff(sn_sum_val[0], "%u events", (unsigned)s->total);

    /* prob: avg / max */
    if (s->total == 0)
        ui_label_set_text_if_diff(sn_sum_val[1], "-");
    else
        ui_label_set_text_fmt_if_diff(sn_sum_val[1], "avg %u%%  max %u%%",
                                      (unsigned)s->avg_prob, (unsigned)s->max_prob);

    /* heavy: prob>=90 的条数 */
    if (s->total == 0)
        ui_label_set_text_if_diff(sn_sum_val[2], "-");
    else
        ui_label_set_text_fmt_if_diff(sn_sum_val[2], "%u of %u",
                                      (unsigned)s->heavy, (unsigned)s->total);

    /* peak: 事件最多的整点时段。23 点要绕回 00，不能显示 24:00。 */
    if (s->peak_count == 0)
        ui_label_set_text_if_diff(sn_sum_val[3], "-");
    else
        ui_label_set_text_fmt_if_diff(sn_sum_val[3], "%02u:00-%02u:00  %u",
                                      (unsigned)s->peak_hour,
                                      (unsigned)((s->peak_hour + 1) % 24),
                                      (unsigned)s->peak_count);

    /* last: 最后一条事件 */
    if (s->last_ts == 0)
    {
        ui_label_set_text_if_diff(sn_sum_val[4], "-");
    }
    else
    {
        rt_uint32_t sod = s->last_ts % 86400;

        rt_snprintf(buf, sizeof(buf), "%02u:%02u  prob %u%%",
                    (unsigned)(sod / 3600), (unsigned)((sod % 3600) / 60),
                    (unsigned)s->last_prob);
        ui_label_set_text_if_diff(sn_sum_val[4], buf);
    }
}

/* ===================== 归档视图总装 ===================== */

/* 归档视图：左图表 + 右汇总。所有控件在这里保证就已建好。 */
static void sn_show_archive(lv_obj_t *parent, const snore_sum_snapshot_t *snap)
{
    rt_uint32_t total = snap->day[sn_day_idx].sum.total;
    char buf[80];
    int i;

    /* 翻页栏：日期 + TODAY + 当天总数 */
    if (total == 0)
        rt_snprintf(buf, sizeof(buf), "%s   no events", snap->date[sn_day_idx]);
    else
        rt_snprintf(buf, sizeof(buf), "%s%s   TOTAL %u",
                    snap->date[sn_day_idx],
                    (sn_day_idx == 0) ? "  TODAY" : "",
                    (unsigned)total);
    ui_label_set_text_if_diff(sn_nav_lbl, buf);

    /* 开机视图的卡片若还在，先撤干净（两个视图互斥） */
    while (sn_rows_used > 0)
    {
        sn_rows_used--;
        if (sn_row_card[sn_rows_used] != RT_NULL)
        {
            lv_obj_delete(sn_row_card[sn_rows_used]);
            sn_row_card[sn_rows_used] = RT_NULL;
            sn_row_time[sn_rows_used] = RT_NULL;
            sn_row_prob[sn_rows_used] = RT_NULL;
            sn_row_cost[sn_rows_used] = RT_NULL;
        }
    }
    if (sn_empty_lbl != RT_NULL)
    {
        lv_obj_delete(sn_empty_lbl);
        sn_empty_lbl = RT_NULL;
    }

    /* 把开机视图隐藏掉的图表骨架恢复出来。
     *
     * 这一步不能省：NTP 校时成功后视图会从 boot 切到 archive，而
     * sn_show_boot 把轴底/基线/刻度/图例都隐藏了。不恢复的话用户看到的
     * 是"时间对了但图表空了"——最难查的一类 bug（数据没问题，是控件
     * 还带着上一帧的隐藏态）。 */
    if (sn_plot_bg != RT_NULL)
    {
        lv_obj_clear_flag(sn_plot_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(sn_axis_line, LV_OBJ_FLAG_HIDDEN);
        for (i = 0; i < 3; i++)
            lv_obj_clear_flag(sn_axis_tick[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(sn_legend_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(sn_legend_val, LV_OBJ_FLAG_HIDDEN);
        ui_label_set_text_if_diff(sn_legend_name, "snore events");
    }
    if (sn_chart_title != RT_NULL)
        ui_label_set_text_if_diff(sn_chart_title, "SNORE");

    /* 汇总卡在 boot 视图被删掉了，这里重建 */
    if (sn_sum_card == RT_NULL)
        sn_sum_build(parent);

    sn_chart_update(snap);
    sn_sum_update(parent, snap);
}

/* ===================== 开机视图（无日历时间） ===================== */

/* 第 i 张事件卡片：白板卡 + 时间/概率/耗时三段标签 */
static void sn_event_card(lv_obj_t *parent, int i)
{
    lv_obj_t *card = ui_card_create(parent, SN_CONTENT_W, SN_BOOT_CARD_H);

    lv_obj_set_pos(card, 0, SN_PLOT_TOP + i * (SN_BOOT_CARD_H + SN_BOOT_CARD_GAP));

    sn_row_time[i] = ui_label(card, &ui_style_small);
    lv_obj_set_pos(sn_row_time[i], 14, 8);

    sn_row_prob[i] = ui_label(card, &ui_style_small);
    lv_obj_set_pos(sn_row_prob[i], 330, 8);

    sn_row_cost[i] = ui_label(card, &ui_style_small_dim);
    lv_obj_set_pos(sn_row_cost[i], 660, 8);

    sn_row_card[i] = card;
}

/* 时钟未同步 / SD 不可用：回退到本次开机的事件环形缓冲 */
static void sn_show_boot(lv_obj_t *parent, const snore_sum_snapshot_t *snap)
{
    snore_rt_hist_t hist[SN_BOOT_ROWS];
    char buf[64];
    int n, i;

    /* 图表与汇总卡都与归档视图互斥：留着会误导成"今天 0 次" */
    if (sn_plot_bg != RT_NULL)
    {
        lv_obj_add_flag(sn_plot_bg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sn_axis_line, LV_OBJ_FLAG_HIDDEN);
        for (i = 0; i < 3; i++)
            lv_obj_add_flag(sn_axis_tick[i], LV_OBJ_FLAG_HIDDEN);
        for (i = 0; i < 4; i++)
            ui_label_set_text_if_diff(sn_tick_lbl[i], "");
        for (i = 0; i < SN_MARK_POOL; i++)
            lv_obj_add_flag(sn_mark[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sn_legend_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(sn_legend_val, LV_OBJ_FLAG_HIDDEN);
        ui_label_set_text_if_diff(sn_legend_name, "");
        ui_label_set_text_if_diff(sn_chart_hint, "");
        sn_last_valid = RT_FALSE;
    }
    if (sn_sum_card != RT_NULL)
    {
        lv_obj_delete(sn_sum_card);
        sn_sum_card = RT_NULL;
        for (i = 0; i < 5; i++)
            sn_sum_val[i] = RT_NULL;
    }

    /* 翻页栏禁用：没有日历时间就没有"昨天/前天"可分 */
    if (sn_nav_lbl != RT_NULL)
    {
        const char *reason = (!snap->time_valid)
                             ? "clock not synced (no NTP)"
                             : "no SD card";

        rt_snprintf(buf, sizeof(buf), "%s", reason);
        ui_label_set_text_if_diff(sn_nav_lbl, buf);
    }

    if (sn_chart_title != RT_NULL)
    {
        const char *title = (!snap->time_valid) ? "SNORE (boot view)"
                                                : "SNORE (no SD)";

        ui_label_set_text_if_diff(sn_chart_title, title);
    }

    n = snore_rt_get_history(hist, SN_BOOT_ROWS);

    for (i = 0; i < n; i++)
    {
        rt_uint32_t s = hist[i].uptime_s;

        if (i >= sn_rows_used)
        {
            sn_event_card(parent, i);
            sn_rows_used++;
        }

        rt_snprintf(buf, sizeof(buf), "+%02u:%02u:%02u",
                    (unsigned)(s / 3600u),
                    (unsigned)((s / 60u) % 60u),
                    (unsigned)(s % 60u));
        ui_label_set_text_if_diff(sn_row_time[i], buf);

        ui_label_set_text_fmt_if_diff(sn_row_prob[i], "prob %u%%",
                                      (unsigned)hist[i].prob);
        ui_label_set_text_fmt_if_diff(sn_row_cost[i], "%ums",
                                      (unsigned)hist[i].cost_ms);
    }

    /* 历史变少（理论上重启才发生）：多余卡片删除，子标签随之回收 */
    while (sn_rows_used > n)
    {
        sn_rows_used--;
        if (sn_row_card[sn_rows_used] != RT_NULL)
        {
            lv_obj_delete(sn_row_card[sn_rows_used]);
            sn_row_card[sn_rows_used] = RT_NULL;
            sn_row_time[sn_rows_used] = RT_NULL;
            sn_row_prob[sn_rows_used] = RT_NULL;
            sn_row_cost[sn_rows_used] = RT_NULL;
        }
    }

    /* 空态占位独立于卡片数组：出现首个事件时删掉 */
    if (n == 0)
    {
        if (sn_empty_lbl == RT_NULL && sn_rows_used == 0)
        {
            sn_empty_lbl = ui_label(parent, &ui_style_small_dim);
            lv_label_set_text(sn_empty_lbl, "  no events yet");
            lv_obj_set_pos(sn_empty_lbl, 2, SN_PLOT_TOP + 6);
        }
    }
    else if (sn_empty_lbl != RT_NULL)
    {
        lv_obj_delete(sn_empty_lbl);
        sn_empty_lbl = RT_NULL;
    }
}

static void snore_draw(lv_obj_t *parent)
{
    snore_rt_status_t      st;
    snore_sum_snapshot_t   snap;
    rt_bool_t              archive_ok;
    lv_coord_t             x = 0;

    snore_rt_get_status(&st);

    /* 首次进入：建静态骨架。判据用 sn_ovw_val[0] 是否为 NULL */
    if (sn_ovw_val[0] == RT_NULL)
    {
        app_tile(parent, x,               "MQTT",      &sn_ovw_val[0]);
        x += APP_TILE_W + 12;
        app_tile(parent, x,               "THRESHOLD", &sn_ovw_val[1]);
        x += APP_TILE_W + 12;
        app_tile(parent, x,               "WINDOWS",   &sn_ovw_val[2]);
        x += APP_TILE_W + 12;
        app_tile(parent, x,               "LAST PROB", &sn_ovw_val[3]);

        /* 翻页栏：左箭头 / 中间信息 / 右箭头 */
        sn_nav_prev = lv_button_create(parent);
        lv_obj_set_size(sn_nav_prev, SN_NAV_ARROW_W, SN_NAV_H);
        lv_obj_set_pos(sn_nav_prev, 0, SN_NAV_TOP);
        lv_obj_set_style_radius(sn_nav_prev, 0, 0);
        lv_obj_set_style_bg_color(sn_nav_prev, lv_color_hex(UI_CARD_BG), 0);
        lv_obj_set_style_border_color(sn_nav_prev, lv_color_hex(UI_CARD_EDGE), 0);
        lv_obj_set_style_border_width(sn_nav_prev, 1, 0);
        lv_obj_set_style_shadow_width(sn_nav_prev, 0, 0);
        {
            lv_obj_t *lab = lv_label_create(sn_nav_prev);

            lv_label_set_text(lab, "<");
            lv_obj_set_style_text_color(lab, lv_color_hex(UI_INK), 0);
            lv_obj_set_style_text_font(lab, UI_FONT_SMALL, 0);
            lv_obj_center(lab);
        }
        lv_obj_add_event_cb(sn_nav_prev, sn_nav_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)-1);

        sn_nav_next = lv_button_create(parent);
        lv_obj_set_size(sn_nav_next, SN_NAV_ARROW_W, SN_NAV_H);
        lv_obj_set_pos(sn_nav_next, SN_CONTENT_W - SN_NAV_ARROW_W, SN_NAV_TOP);
        lv_obj_set_style_radius(sn_nav_next, 0, 0);
        lv_obj_set_style_bg_color(sn_nav_next, lv_color_hex(UI_CARD_BG), 0);
        lv_obj_set_style_border_color(sn_nav_next, lv_color_hex(UI_CARD_EDGE), 0);
        lv_obj_set_style_border_width(sn_nav_next, 1, 0);
        lv_obj_set_style_shadow_width(sn_nav_next, 0, 0);
        {
            lv_obj_t *lab = lv_label_create(sn_nav_next);

            lv_label_set_text(lab, ">");
            lv_obj_set_style_text_color(lab, lv_color_hex(UI_INK), 0);
            lv_obj_set_style_text_font(lab, UI_FONT_SMALL, 0);
            lv_obj_center(lab);
        }
        lv_obj_add_event_cb(sn_nav_next, sn_nav_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)1);

        sn_nav_lbl = ui_label(parent, &ui_style_small);
        lv_label_set_text(sn_nav_lbl, "-");
        lv_obj_set_pos(sn_nav_lbl, SN_NAV_ARROW_W + 12,
                       SN_NAV_TOP + (SN_NAV_H - 16) / 2);

        sn_chart_build(parent);
        sn_sum_build(parent);

        sn_rows_used = 0;
        sn_last_valid = RT_FALSE;
    }

    /* 概览数值 */
    ui_label_set_text_if_diff(sn_ovw_val[0], st.mqtt_up ? "ON" : "OFF");
    ui_label_set_text_fmt_if_diff(sn_ovw_val[1], "%u%%", (unsigned)st.threshold);
    ui_label_set_text_fmt_if_diff(sn_ovw_val[2], "%u", (unsigned)st.windows);
    ui_label_set_text_fmt_if_diff(sn_ovw_val[3], "%u%%", (unsigned)st.last_prob);

    /* 数据本体：只读后台线程的内存快照，绝不在这里碰文件系统 */
    archive_ok = snore_sum_get(&snap);

    /* 索引可能在时间失效后被越界使用（快照重置），夹一下 */
    if (sn_day_idx >= SNORE_SUM_DAYS)
        sn_day_idx = 0;

    /* 翻页到边界时把箭头置灰，避免用户以为点击无效。
     *
     * 只在状态确实需要变化时才调 add/remove_state：这两个 API 无条件
     * invalidate，draw 每 500ms 跑一次，无条件调用会让箭头持续重绘。
     * lv_obj_remove_state(obj, 0) 也不做空操作保护，传 0 是未定义用法。 */
    if (sn_nav_prev != RT_NULL && sn_nav_next != RT_NULL)
    {
        rt_bool_t prev_off = (!archive_ok || sn_day_idx == 0);
        rt_bool_t next_off = (!archive_ok || sn_day_idx == SNORE_SUM_DAYS - 1);

        if (prev_off != sn_nav_prev_off)
        {
            if (prev_off)
                lv_obj_add_state(sn_nav_prev, LV_STATE_DISABLED);
            else
                lv_obj_remove_state(sn_nav_prev, LV_STATE_DISABLED);
            sn_nav_prev_off = prev_off;
        }

        if (next_off != sn_nav_next_off)
        {
            if (next_off)
                lv_obj_add_state(sn_nav_next, LV_STATE_DISABLED);
            else
                lv_obj_remove_state(sn_nav_next, LV_STATE_DISABLED);
            sn_nav_next_off = next_off;
        }
    }

    if (archive_ok)
        sn_show_archive(parent, &snap);
    else
        sn_show_boot(parent, &snap);
}

static void snore_undraw(lv_obj_t *parent)
{
    int i;

    RT_UNUSED(parent);

    for (i = 0; i < SN_BOOT_ROWS; i++)
    {
        sn_row_card[i] = RT_NULL;
        sn_row_time[i] = RT_NULL;
        sn_row_prob[i] = RT_NULL;
        sn_row_cost[i] = RT_NULL;
    }
    for (i = 0; i < 4; i++)
    {
        sn_ovw_val[i] = RT_NULL;
        sn_tick_lbl[i] = RT_NULL;
    }
    for (i = 0; i < 3; i++)
        sn_axis_tick[i] = RT_NULL;
    for (i = 0; i < SN_MARK_POOL; i++)
        sn_mark[i] = RT_NULL;
    for (i = 0; i < 5; i++)
        sn_sum_val[i] = RT_NULL;

    sn_nav_prev    = RT_NULL;
    sn_nav_next    = RT_NULL;
    sn_nav_lbl     = RT_NULL;
    sn_chart_title = RT_NULL;
    sn_plot_bg     = RT_NULL;
    sn_axis_line   = RT_NULL;
    sn_legend_box  = RT_NULL;
    sn_legend_name = RT_NULL;
    sn_legend_val  = RT_NULL;
    sn_chart_hint  = RT_NULL;
    sn_sum_card    = RT_NULL;
    sn_rows_used   = 0;
    sn_empty_lbl   = RT_NULL;
    sn_mark_used   = 0;
    /* 控件已随 parent 删除，重建时的初始状态是"未禁用"，
     * 缓存必须跟着复位，否则新箭头永远不会被置灰。 */
    sn_nav_prev_off = -1;
    sn_nav_next_off = -1;
    /* 位图缓存也要清：新控件的标记位置从未设置过 */
    sn_last_valid = RT_FALSE;
    /* sn_day_idx 故意不复位：与 ui_lanctl.c 的 lui_sel 同理，
     * 用户翻到昨天看完退出，再进来还应该在昨天。
     * 控件本身随 parent 一起清空，这里只清指针，避免悬空引用 */
}

/* ---------------- AI Chat：全屏聊天界面 ----------------
 *
 * 数据来自 app_data 的小智通道（xiaozhi_seq / role / text / status），
 * 由 xiaozhi_port.c 写入。这里只读，靠 ui_app 的 500ms tick 拉取，
 * 不再自建 timer（旧 ui_chat.c 是 200ms 自建 timer + 建在 lv_scr_act
 * 上的全屏页，与本框架的 draw(parent) 契约不兼容）。
 *
 * 布局：状态行 + 可滚动消息流。app_body 本身不可滚动且已带 24px padding，
 * 所以消息流要自建一个可滚动的子容器。
 */

#define XZ_KEEP_MAX   12        /* 保留气泡数，超出删最旧 */
#define XZ_ROLE_USER  0
#define XZ_ROLE_AI    1

/* 8 段电平条：比连续进度条更像"对讲机在收音"，
 * 也比单个圆点更容易看出麦克风到底有没有在收数据。 */
#define XZ_BAR_SEG    8

static lv_obj_t *xz_status_lbl = RT_NULL;
static lv_obj_t *xz_list      = RT_NULL;   /* 滚动消息容器 */
static lv_obj_t *xz_hint      = RT_NULL;   /* 底部操作提示 */
static lv_obj_t *xz_dot       = RT_NULL;   /* 状态圆点 */
static lv_obj_t *xz_bar[XZ_BAR_SEG] = {0}; /* 电平条 */
static lv_obj_t *xz_bar_hint  = RT_NULL;   /* "LISTENING" / "SPEAKING" */
static lv_obj_t *xz_ptt       = RT_NULL;   /* 右下角"点击说话"按钮 */
static lv_obj_t *xz_ptt_txt   = RT_NULL;   /* 按钮上的文字 */
static int xz_bubble_cnt = 0;
static rt_uint8_t xz_last_seg = 0xFF;
static rt_uint8_t xz_last_state = 0xFF;

/* 按钮当前已绘制的外观（-1 表示尚未绘制）。
 *
 * 点击切换模式下，按钮外观完全由 500ms 的状态刷新决定，而不由点击事件
 * 自行绘制，否则"点击绘制一次、状态刷新再绘制一次"会互相覆盖，导致断连、
 * 服务端收尾等状态变化无法反映到按钮上。缓存用于避免每 500ms 无条件写
 * 样式而触发重绘。 */
#define XZ_PTT_IDLE      0     /* 可点，未录音 */
#define XZ_PTT_REC       1     /* 正在录音 */
#define XZ_PTT_DISABLED  2     /* 小智未启动，不可用 */
static rt_int8_t xz_ptt_painted = -1;

/* xiaozhi_audio.cpp 提供（无独立头文件，前置声明） */
extern rt_uint32_t xz_mic_rms_get(void);

/* 状态点颜色与电平条活跃色：按设备状态取值。
 *   Idle(3)       灰   已连接待命
 *   Connecting(4) 橙   正在连接
 *   Listening(5)  红   正在收音
 *   Speaking(6)   青   AI 正在回话（本工程无 TTS 音频，但文字在推送）
 *   其它          灰   未启动或异常
 */
static rt_uint32_t xz_state_color(int st)
{
    switch (st)
    {
    case 4:  return UI_ATTENTION;      /* Connecting */
    case 5:  return 0xD9534F;          /* Listening 柔红 */
    case 6:  return UI_MEMO_KNOWN;     /* Speaking  柔青绿 */
    default: return UI_MUTED;
    }
}

static const char *xz_state_word(int st, rt_bool_t running, rt_bool_t connected)
{
    if (!running)
        return "STOPPED";
    if (!connected)
        return "CONNECTING";
    switch (st)
    {
    case 5:  return "LISTENING";
    case 6:  return "SPEAKING";
    default: return "READY";
    }
}

/* 刷新图形反馈。仅在状态或段数确实变化时才操作控件，因为该页面每 500ms
 * 刷新一次，每次无条件写样式都会触发全屏重绘（768KB 缓存清理加 VG-Lite
 * 旋转），明显影响滑动手感。 */
static void xz_refresh_indicator(void)
{
    rt_uint8_t seg;
    rt_uint8_t i;
    rt_uint32_t color;
    int st;
    rt_bool_t connected;
    rt_bool_t running;
    xz_app_status_t s;

    if (xz_dot == RT_NULL || xz_bar_hint == RT_NULL)
        return;

    xz_app_get_status(&s);
    running   = s.running;
    connected = s.ws_connected;
    /* s.state 是 DeviceState 枚举值，见 xiaozhi.h */
    st = (int)s.state;

    /* 用 s.listening（mic0 实际开着）而不是只看状态枚举：状态机切到
     * Listening 和服务端真正开始收音频之间有几十毫秒的窗口，而且设备
     * 在 Speaking 期间麦克风是关的。以硬件开关为准，用户看到的条子才
     * 和"现在说话有没有被录进去"一致。 */
    if (s.listening)
    {
        seg = rms_to_level(xz_mic_rms_get()) * XZ_BAR_SEG / 100;
    }
    else
    {
        seg = 0;
    }

    if (seg != xz_last_seg || st != xz_last_state)
    {
        color = xz_state_color(st);

        for (i = 0; i < XZ_BAR_SEG; i++)
        {
            if (xz_bar[i] == RT_NULL)
                continue;

            if (i < seg)
            {
                lv_obj_set_style_bg_color(xz_bar[i], lv_color_hex(color), 0);
                lv_obj_set_style_bg_opa(xz_bar[i], LV_OPA_COVER, 0);
            }
            else
            {
                lv_obj_set_style_bg_color(xz_bar[i], lv_color_hex(UI_MUTED), 0);
                lv_obj_set_style_bg_opa(xz_bar[i], LV_OPA_30, 0);
            }
        }

        lv_obj_set_style_bg_color(xz_dot, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(xz_dot,
                                running ? LV_OPA_COVER : LV_OPA_30, 0);

        ui_label_set_text_if_diff(xz_bar_hint,
                                  xz_state_word(st, running, connected));

        xz_last_seg = seg;
        xz_last_state = (rt_uint8_t)st;
    }
}

/* ---------------- 点击说话（触摸） ---------------- */

/* 按状态绘制按钮。三种外观：
 *   DISABLED  灰色描边加 "START FIRST"   小智未启动，不可点击
 *   REC       墨黑实心加 "TAP TO STOP"   正在录音
 *   IDLE      白底描边加 "TAP TO TALK"   可点击
 *
 * 仅在状态确实变化时才写样式：该页面每 500ms 刷新一次，无条件写入会触发
 * 重绘（全屏 VG-Lite 旋转），导致按钮闪烁且滑动变卡。 */
static void xz_ptt_paint(rt_int8_t mode)
{
    const char *text;
    rt_uint32_t bg;
    rt_uint32_t fg;
    rt_uint32_t edge;

    if (xz_ptt == RT_NULL || mode == xz_ptt_painted)
        return;

    switch (mode)
    {
    case XZ_PTT_REC:
        bg = UI_INK;        fg = UI_CARD_BG;  edge = UI_INK;
        text = "TAP TO STOP";
        break;
    case XZ_PTT_DISABLED:
        bg = UI_CARD_BG;    fg = UI_MUTED;    edge = UI_CARD_EDGE;
        text = "START FIRST";
        break;
    default:
        bg = UI_CARD_BG;    fg = UI_INK;      edge = UI_CARD_EDGE;
        text = "TAP TO TALK";
        break;
    }

    lv_obj_set_style_bg_color(xz_ptt, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(xz_ptt, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(xz_ptt, lv_color_hex(edge), 0);

    if (xz_ptt_txt != RT_NULL)
    {
        lv_obj_set_style_text_color(xz_ptt_txt, lv_color_hex(fg), 0);
        ui_label_set_text_if_diff(xz_ptt_txt, text);
    }

    xz_ptt_painted = mode;
}

/* 点击切换：点击一次开启麦克风，再次点击停止。
 *
 * 使用 CLICKED 而非 PRESSED 或 RELEASED 的原因：此处已不是按住说话，
 * 一次点击即构成完整的切换动作。CLICKED 仅在按下并在同一控件上抬起时
 * 触发，天然过滤误触与滑动，也无需再处理 PRESS_LOST；而在按住模式下，
 * 该事件正是导致"麦克风一直开着"的原因，此处不存在该问题。
 *
 * 实际开停以 s.listening（mic0 是否真正打开）为准，不自行维护"期望开启
 * 还是关闭"的标志：服务端可能因 VAD 或超时自行收尾，断连时状态机也会
 * 关闭麦克风；一旦本地标志与服务端不同步，按钮就会显示"可说话"而麦克风
 * 实际开着，或反之。 */
static void xz_ptt_event_cb(lv_event_t *e)
{
    xz_app_status_t s;

    RT_UNUSED(e);

    xz_app_get_status(&s);

    if (!s.running)
    {
        /* 未启动：事件组还不存在，投递会失败。直接提示，不静默。 */
        xz_ptt_paint(XZ_PTT_DISABLED);
        return;
    }

    if (s.listening)
    {
        xz_app_touch_release();      /* 发 listen stop，关麦 */
    }
    else
    {
        if (xz_app_touch_press() != 0)
        {
            xz_ptt_paint(XZ_PTT_DISABLED);
            return;
        }
    }

    /* 立刻刷一次：状态机切状态要几十毫秒，但按钮应该马上响应点击。
     * 清缓存是为了让 paint 一定重画（否则同一个 mode 会被跳过）。 */
    xz_ptt_painted = -1;
    xz_last_seg   = 0xFF;
    xz_last_state = 0xFF;
    xz_ptt_paint(s.listening ? XZ_PTT_IDLE : XZ_PTT_REC);
}

/* 追加一条气泡：用户墨黑右对齐，AI 白卡左对齐。复用旧 ui_chat 的做法。 */
static void xz_add_bubble(rt_uint8_t role, const char *text)
{
    lv_obj_t *row;
    lv_obj_t *spring_a;
    lv_obj_t *bubble;
    lv_obj_t *label;
    lv_obj_t *spring_b;
    lv_coord_t list_w;

    if (xz_list == RT_NULL || text == RT_NULL || text[0] == '\0')
        return;

    list_w = lv_obj_get_content_width(xz_list);

    row = lv_obj_create(xz_list);
    lv_obj_set_width(row, list_w);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 左侧弹簧：用户消息时撑开，把气泡顶到右边 */
    spring_a = lv_obj_create(row);
    lv_obj_set_size(spring_a, 1, 1);
    lv_obj_set_style_bg_opa(spring_a, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spring_a, 0, 0);
    lv_obj_clear_flag(spring_a, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_grow(spring_a, role == XZ_ROLE_USER ? 1 : 0);

    bubble = (role == XZ_ROLE_USER) ? ui_card_dark_create(row, 1, 1)
                                    : ui_card_create(row, 1, 1);
    lv_obj_set_width(bubble, LV_SIZE_CONTENT);
    lv_obj_set_height(bubble, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bubble, 10, 0);

    label = ui_label(bubble, &ui_style_text);
    lv_label_set_text(label, text);
    /* ui_style_text 在字库加载后指向 cjk16（GB2312），ASCII 靠 fallback
     * 落到 montserrat_16。所以中文和英文都能画出来。 */
    lv_obj_set_style_max_width(label, list_w * 7 / 10, 0);
    if (role == XZ_ROLE_USER)
        lv_obj_set_style_text_color(label, lv_color_hex(UI_CARD_BG), 0);

    /* 右侧弹簧：AI 消息时撑开，把气泡留在左边 */
    spring_b = lv_obj_create(row);
    lv_obj_set_size(spring_b, 1, 1);
    lv_obj_set_style_bg_opa(spring_b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spring_b, 0, 0);
    lv_obj_clear_flag(spring_b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_grow(spring_b, role == XZ_ROLE_AI ? 1 : 0);

    xz_bubble_cnt++;
    while (xz_bubble_cnt > XZ_KEEP_MAX)
    {
        lv_obj_t *oldest = lv_obj_get_child(xz_list, 0);

        if (oldest == RT_NULL)
            break;
        lv_obj_delete(oldest);
        xz_bubble_cnt--;
    }

    lv_obj_scroll_to_y(xz_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void xz_draw(lv_obj_t *parent)
{
    xz_app_status_t st;
    rt_uint8_t role;
    char text[APP_DATA_XZ_TEXT_LEN];

    /* 首次进入时建立骨架：判断依据是 xz_list 为空指针。
     * 不能通过解引用旧指针判断 parent，因为 ui_app 已执行过 lv_obj_clean。 */
    if (xz_list == RT_NULL)
    {
        lv_coord_t y;

        /* ---- 状态指示区（26px 高）----
         *    ● LISTENING  ▁▃▅▇▅▃▁    右侧为文字状态
         *
         * 此前此处仅有 "-" 文本行，用户无法判断麦克风是否开启；
         * 按住侧键说话时屏幕没有任何变化，只有气泡出现才能确认已录入。 */
        xz_dot = lv_obj_create(parent);
        lv_obj_set_size(xz_dot, 12, 12);
        lv_obj_set_pos(xz_dot, 2, 7);
        lv_obj_set_style_radius(xz_dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(xz_dot, lv_color_hex(UI_MUTED), 0);
        lv_obj_set_style_bg_opa(xz_dot, LV_OPA_30, 0);
        lv_obj_set_style_border_width(xz_dot, 0, 0);
        lv_obj_clear_flag(xz_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        /* 8 段电平条。每段 8x14，间距 4，共 92px */
        for (int i = 0; i < XZ_BAR_SEG; i++)
        {
            xz_bar[i] = lv_obj_create(parent);
            lv_obj_set_size(xz_bar[i], 8, 14);
            lv_obj_set_pos(xz_bar[i], 22 + i * 12, 6);
            lv_obj_set_style_radius(xz_bar[i], 2, 0);
            lv_obj_set_style_bg_color(xz_bar[i], lv_color_hex(UI_MUTED), 0);
            lv_obj_set_style_bg_opa(xz_bar[i], LV_OPA_30, 0);
            lv_obj_set_style_border_width(xz_bar[i], 0, 0);
            lv_obj_clear_flag(xz_bar[i],
                              LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        }

        xz_bar_hint = ui_label(parent, &ui_style_small_dim);
        lv_label_set_text(xz_bar_hint, "STOPPED");
        lv_obj_set_pos(xz_bar_hint, 128, 3);

        /* 原状态行（connecting... / listening 等，来自 xz_app_get_status）
         * 保留在右侧作为补充信息：图形区仅表达"能否说话"，
         * 该行仍可看出 running 与 connected 的细节。 */
        xz_status_lbl = ui_label(parent, &ui_style_small_dim);
        lv_label_set_text(xz_status_lbl, "-");
        lv_obj_set_pos(xz_status_lbl, 300, 3);
        lv_obj_set_width(xz_status_lbl, 400);

        y = 30;

        xz_list = lv_obj_create(parent);
        lv_obj_set_size(xz_list, LV_PCT(100), LV_PCT(100));
        lv_obj_set_pos(xz_list, 0, y);
        lv_obj_set_style_bg_opa(xz_list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(xz_list, 0, 0);
        lv_obj_set_style_radius(xz_list, 0, 0);
        lv_obj_set_style_pad_all(xz_list, 4, 0);
        lv_obj_set_style_pad_row(xz_list, 8, 0);
        lv_obj_set_flex_flow(xz_list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(xz_list, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_scroll_dir(xz_list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(xz_list, LV_SCROLLBAR_MODE_AUTO);

        /* app_body 高度固定，消息区占满剩余空间：
         * 顶部让出 30px 给状态指示区，底部让出 18px 给提示行。
         *
         * 按钮已经挪到右下角悬浮，不再占用列表底部的整条高度，
         * 所以列表可以一直铺到提示行上方。 */
        lv_obj_set_height(xz_list,
                          lv_obj_get_content_height(parent) - y - 18);

        /* ---- 右下角"点击说话"按钮 ----
         *
         * 长方形悬浮于消息区右下角。点击切换录音：点击一次开启，
         * 再次点击停止，无需按住。
         *
         * 尺寸 168x48：拇指可及，又不至于遮挡气泡。距离右边与下边各 8px，
         * 贴近屏边但保留间隙，与 VG-Lite 的矩形路径对齐。
         *
         * 在列表之后创建的原因：LVGL 中后创建的对象层级更高，
         * 按钮必须位于滚动列表之上才能接收点击。 */
        xz_ptt = lv_obj_create(parent);
        lv_obj_set_size(xz_ptt, 168, 48);
        lv_obj_align(xz_ptt, LV_ALIGN_BOTTOM_RIGHT, -8, -24);
        lv_obj_set_style_radius(xz_ptt, 2, 0);
        lv_obj_set_style_border_width(xz_ptt, 1, 0);
        lv_obj_set_style_pad_all(xz_ptt, 0, 0);
        lv_obj_clear_flag(xz_ptt, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(xz_ptt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(xz_ptt, xz_ptt_event_cb, LV_EVENT_CLICKED, RT_NULL);

        xz_ptt_txt = ui_label(xz_ptt, &ui_style_small);
        lv_label_set_text(xz_ptt_txt, "TAP TO TALK");
        lv_obj_center(xz_ptt_txt);

        /* 底部提示行：说明当前可执行的操作。 */
        xz_hint = ui_label(parent, &ui_style_small_dim);
        lv_label_set_text(xz_hint, "");
        lv_obj_set_pos(xz_hint, 4,
                       lv_obj_get_content_height(parent) - 18);
        lv_obj_set_width(xz_hint, 480);   /* 不与右下角按钮争用位置 */
        lv_obj_set_style_text_color(xz_hint, lv_color_hex(UI_MUTED), 0);

        /* 首次进入时按当前状态绘制一次，避免先白后黑的闪烁 */
        xz_ptt_painted = -1;
        xz_bubble_cnt = 0;
        xz_last_seg   = 0xFF;
        xz_last_state = 0xFF;
    }

    xz_app_get_status(&st);
    ui_label_set_text_if_diff(xz_status_lbl, st.status);
    xz_refresh_indicator();

    /* 按钮外观完全以实际状态为准，每 500ms 同步一次。
     *
     * 这是点击切换模式下唯一的按钮绘制入口（点击回调只负责投递事件）。
     * 该设计确保按钮始终反映真实状态：服务端 VAD 自行收尾、断连导致状态机
     * 关闭麦克风、甚至实体侧键开关麦克风，按钮都会在半个周期内同步，
     * 不会出现"按钮显示 TAP TO STOP 而麦克风实际已关闭"的情况。
     *
     * xz_ptt_paint 内部有 mode 缓存，状态未变化时不写入任何内容。 */
    if (!st.running)
        xz_ptt_paint(XZ_PTT_DISABLED);
    else if (st.listening)
        xz_ptt_paint(XZ_PTT_REC);
    else
        xz_ptt_paint(XZ_PTT_IDLE);

    /* 提示行：只在没有占用下半屏的情况下给一句引导。
     * 按钮本身已经写明可做的动作，这里补充"为什么现在点不了"。 */
    if (xz_hint != RT_NULL)
    {
        const char *tip;

        if (!st.running)
            tip = "Tap START (top right) to begin";
        else if (!st.ws_connected)
            tip = "Connecting to server...";
        else if (st.listening)
            tip = "Recording - tap the button again to send";
        else
            tip = "";

        ui_label_set_text_if_diff(xz_hint, tip);
    }

    /* 排空队列：一个 tick 内可能到达多条消息（TTS 逐句推送
     * sentence_start，而 ui_app 为 500ms 一次）。原先只取一条并用 seq
     * 比较，多余消息会被下一条覆盖，对话中表现为"跳句"。此处循环取至空。
     *
     * 每取到一条即续期息屏：用户正在查看对话，屏幕不应变黑。
     * （ui_app 每次 draw 也会触发 activity，但那是 500ms 一次；
     * 消息到达属于离散事件，单独续期更及时。） */
    while (app_data_xz_pop(&role, text, sizeof(text)))
    {
        xz_add_bubble(role, text);
        screen_saver_notify_activity();
    }
}

static void xz_undraw(lv_obj_t *parent)
{
    RT_UNUSED(parent);

    /* 只清指针：控件本体由 ui_app 的 lv_obj_clean 统一删除。
     * 保留旧指针会在下次 draw 时被当作有效对象解引用。
     *
     * 注意电平条为数组，必须逐个清空。若只清 xz_bar[0]，下次 xz_draw
     * 判断 xz_list == NULL 时会重建全部 8 条，但旧数组中后 7 个仍是
     * 已释放地址，xz_refresh_indicator 将向野指针写入样式。 */
    xz_status_lbl = RT_NULL;
    xz_list       = RT_NULL;
    xz_hint       = RT_NULL;
    xz_dot        = RT_NULL;
    xz_bar_hint   = RT_NULL;
    xz_ptt        = RT_NULL;
    xz_ptt_txt    = RT_NULL;
    for (int i = 0; i < XZ_BAR_SEG; i++)
        xz_bar[i] = RT_NULL;

    /* 点击切换模式下退出应用不需要补发任何事件：一次点击即构成完整的
     * 开关动作，不存在"按下一半"的中间态。但录音可能在退出时仍在进行，
     * 那是用户自行点开的，保持原状（回到会话后继续录符合预期，
     * 强制关闭会使"切走查看信息再返回"变为重新按下）。
     * 只需让下次进入时重绘按钮。 */
    xz_ptt_painted = -1;

    xz_bubble_cnt = 0;
    xz_last_seg   = 0xFF;
    xz_last_state = 0xFF;
}

/* ---------------- 应用 2：音频电平事件 ---------------- */


static rt_bool_t audio_is_running(void)
{
    audio_event_status_t st;

    audio_event_get_status(&st);
    return st.running;
}

static rt_uint8_t audio_get_level(void)
{
    audio_event_status_t st;

    audio_event_get_status(&st);
    if (!st.running)
        return 0;

    return rms_to_level(st.rms_last);
}

static void audio_get_status(char *buf, int len)
{
    audio_event_status_t st;

    audio_event_get_status(&st);

    if (!st.running)
    {
        rt_snprintf(buf, len, "Idle   th %u", (unsigned)st.threshold);
        return;
    }

    rt_snprintf(buf, len, "rms %u  ev %u  th %u",
                (unsigned)st.rms_last,
                (unsigned)st.events,
                (unsigned)st.threshold);
}

static void audio_get_detail(char *buf, int len)
{
    audio_event_status_t st;

    audio_event_get_status(&st);

    rt_snprintf(buf, len,
                "State      %s\n"
                "MQTT       %s\n"
                "RMS        %u\n"
                "Threshold  %u\n"
                "Events     %u\n"
                "Topic      wfizzz/sound",
                st.running ? "Running" : "Stopped",
                st.mqtt_connected ? "Online"
                                  : (st.mqtt_started ? "Connecting" : "Off"),
                (unsigned)st.rms_last,
                (unsigned)st.threshold,
                (unsigned)st.events);
}

/* ---------------- Audio 应用屏幕（全屏） ----------------
 *
 * 布局：概览行（RMS、阈值、事件数）与实时电平条。
 * 电平条使用对数刻度（rms_to_level），按 200ms 单独刷新，因为 ui_app 的
 * 内容节拍为 500ms，电平条按该节奏跳变过于迟钝；timer 随 undraw 删除。
 */

#define AU_BAR_W   (APP_TILE_W * 3 + 12 * 2)   /* 552，与概览行同宽 */
#define AU_BAR_H   14

static lv_obj_t  *au_val[3];         /* RMS / 阈值 / 事件数 */
static lv_obj_t  *au_fill;           /* 电平条填充 */
static lv_timer_t *au_timer;

static void au_level_set(rt_uint8_t lvl)
{
    lv_coord_t w = (lv_coord_t)(AU_BAR_W * lvl / 100);

    lv_obj_set_width(au_fill, w);
    if (w > 0)
        lv_obj_clear_flag(au_fill, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(au_fill, LV_OBJ_FLAG_HIDDEN);
}

static void au_level_timer_cb(lv_timer_t *t)
{
    audio_event_status_t st;

    RT_UNUSED(t);

    if (au_val[0] == RT_NULL)
        return;

    audio_event_get_status(&st);
    au_level_set(st.running ? audio_get_level() : 0);
}

static void audio_draw(lv_obj_t *parent)
{
    audio_event_status_t st;

    audio_event_get_status(&st);

    if (au_val[0] == RT_NULL)
    {
        lv_obj_t *track;
        lv_coord_t x = 0;

        app_tile(parent, x, "RMS", &au_val[0]);
        x += APP_TILE_W + 12;
        app_tile(parent, x, "THRESHOLD", &au_val[1]);
        x += APP_TILE_W + 12;
        app_tile(parent, x, "EVENTS", &au_val[2]);

        /* 电平条：UI_SEP 空轨道 + 墨黑填充（矩形改宽，切合直角主题） */
        track = lv_obj_create(parent);
        lv_obj_set_size(track, AU_BAR_W, AU_BAR_H);
        lv_obj_set_pos(track, 0, APP_TILE_H + 22);
        lv_obj_set_style_bg_color(track, lv_color_hex(UI_SEP), 0);
        lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(track, 0, 0);
        lv_obj_set_style_radius(track, 0, 0);
        lv_obj_set_style_pad_all(track, 0, 0);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);

        au_fill = lv_obj_create(track);
        lv_obj_set_size(au_fill, 0, AU_BAR_H);
        lv_obj_set_style_bg_color(au_fill, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_bg_opa(au_fill, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(au_fill, 0, 0);
        lv_obj_set_style_radius(au_fill, 0, 0);
        lv_obj_set_style_pad_all(au_fill, 0, 0);
        lv_obj_add_flag(au_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(au_fill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(au_fill, LV_OBJ_FLAG_CLICKABLE);

        au_timer = lv_timer_create(au_level_timer_cb, 200, RT_NULL);
    }

    if (st.running)
        ui_label_set_text_fmt_if_diff(au_val[0], "%u", (unsigned)st.rms_last);
    else
        ui_label_set_text_if_diff(au_val[0], "-");
    ui_label_set_text_fmt_if_diff(au_val[1], "%u", (unsigned)st.threshold);
    ui_label_set_text_fmt_if_diff(au_val[2], "%u", (unsigned)st.events);

    au_level_set(st.running ? audio_get_level() : 0);
}

static void audio_undraw(lv_obj_t *parent)
{
    int i;

    RT_UNUSED(parent);

    if (au_timer != RT_NULL)
    {
        lv_timer_delete(au_timer);
        au_timer = RT_NULL;
    }
    for (i = 0; i < 3; i++)
        au_val[i] = RT_NULL;
    au_fill = RT_NULL;
}

/* ---------------- 应用 3：模型自检（只读） ---------------- */

static rt_bool_t model_is_running(void)
{
    /* 模型没有"运行"概念，用 arena 是否就绪代替 */
    return snore_server_is_infer_ready();
}

static void model_get_status(char *buf, int len)
{
    snore_server_get_arena_text(buf, len);
}

static void model_get_detail(char *buf, int len)
{
    snore_info_t info;

    snore_infer_get_info(&info);

    if (!info.ok)
    {
        rt_snprintf(buf, len,
                    "State      Not loaded\n"
                    "Hint       run snore_selftest\n"
                    "           or snore_rt start");
        return;
    }

    /* scale 是浮点，但 rt_vsnprintf 不支持 %f，整数化成 Xe-4 显示 */
    rt_snprintf(buf, len,
                "Arena      %u KB\n"
                "Location   %s\n"
                "Input      scale %de-4 zp %d\n"
                "Output     scale %de-4 zp %d\n"
                "TCP server %s",
                (unsigned)(info.arena_size / 1024),
                ((uintptr_t)info.arena_addr & 0xFF000000u) == 0x64000000u
                    ? "private HyperRAM" : "internal SRAM",
                (int)(info.in_scale * 10000.0), info.in_zp,
                (int)(info.out_scale * 10000.0), info.out_zp,
                snore_server_is_running() ? "listening :9000" : "stopped");
}

/* ---------------- Model 应用屏幕（全屏，只读） ---------------- */

static lv_obj_t *mi_text;

static void model_draw(lv_obj_t *parent)
{
    char buf[288];

    model_get_detail(buf, sizeof(buf));

    if (mi_text == RT_NULL)
    {
        mi_text = ui_label(parent, &ui_style_text);
        lv_obj_set_pos(mi_text, 0, 8);
        lv_label_set_text(mi_text, buf);
        return;
    }

    ui_label_set_text_if_diff(mi_text, buf);
}

static void model_undraw(lv_obj_t *parent)
{
    RT_UNUSED(parent);

    mi_text = RT_NULL;
}

/* ---------------- 应用表 ---------------- */

/* ---------------- AI Chat 的状态包装 ----------------
 * 界面实体（xz_draw/xz_undraw）在上面，这里只做 app_entry_t 要求的
 * is_running / get_status 适配。 */
static rt_bool_t xz_is_running(void)
{
    return xz_app_is_running();
}

static void xz_get_status(char *buf, int len)
{
    xz_app_status_t st;

    xz_app_get_status(&st);
    rt_snprintf(buf, len, "%s", st.status);
}

static const app_entry_t s_apps[] =
{
    {
        .name       = "Snore Detect",
        .desc       = "mic -> YAMNet -> MQTT",
        .icon       = APP_ICON_SNORE,
        .uses_mic   = RT_TRUE,
        .is_running = snore_is_running,
        .start      = snore_rt_start,
        .stop       = snore_rt_stop,
        .get_status = snore_get_status,
        .get_detail = snore_get_detail,
        .get_level  = snore_get_level,
        .draw       = snore_draw,
        .undraw     = snore_undraw,
    },
    {
        .name       = "Audio Level",
        .desc       = "mic RMS event test",
        .icon       = APP_ICON_AUDIO,
        .uses_mic   = RT_TRUE,
        .is_running = audio_is_running,
        .start      = audio_event_start,
        .stop       = audio_event_stop,
        .get_status = audio_get_status,
        .get_detail = audio_get_detail,
        .get_level  = audio_get_level,
        .draw       = audio_draw,
        .undraw     = audio_undraw,
    },
    {
        .name       = "AI Chat",
        .desc       = "voice chat via xiaozhi",
        .icon       = APP_ICON_CHAT,
        .uses_mic   = RT_TRUE,
        .is_running = xz_is_running,
        .start      = xz_app_start,
        .stop       = xz_app_stop,
        .get_status = xz_get_status,
        .get_detail = RT_NULL,
        .draw       = xz_draw,
        .undraw     = xz_undraw,
        .get_level  = RT_NULL,
    },
    {
        .name       = "Model Info",
        .desc       = "arena and quantization",
        .icon       = APP_ICON_MODEL,
        .uses_mic   = RT_FALSE,
        .is_running = model_is_running,
        .start      = RT_NULL,          /* 只读应用：不画按钮 */
        .stop       = RT_NULL,
        .get_status = model_get_status,
        .get_detail = model_get_detail,
        .draw       = model_draw,
        .undraw     = model_undraw,
    },
    {
        .name       = "LAN Devices",
        .desc       = "MQTT device control",
        .icon       = APP_ICON_LAN,
        .uses_mic   = RT_FALSE,
        .is_running = lanctl_is_running,
        .start      = lanctl_start,
        .stop       = lanctl_stop,
        .get_status = lanctl_get_status,
        .get_detail = RT_NULL,
        .draw       = ui_lanctl_draw,
        .undraw     = ui_lanctl_undraw,
        .get_level  = RT_NULL,
    },
};

#define APP_NUM  ((int)(sizeof(s_apps) / sizeof(s_apps[0])))

const app_entry_t *app_registry_get(int *count)
{
    if (count != RT_NULL)
        *count = APP_NUM;
    return s_apps;
}

/*
 * mic0 互斥：drv_pdm 只有一个录音设备，两个模块同时 open 会失败。
 * 启动前把其他占用麦克风且正在跑的应用停掉；停止是异步的，
 * 这里短等一小会儿让线程真正退出，避免 open 抢不到。
 */
static void release_other_mic_users(int keep)
{
    int i;

    for (i = 0; i < APP_NUM; i++)
    {
        if (i == keep || !s_apps[i].uses_mic)
            continue;
        if (s_apps[i].is_running == RT_NULL || !s_apps[i].is_running())
            continue;
        if (s_apps[i].stop == RT_NULL)
            continue;

        LOG_I("stopping %s to free mic0", s_apps[i].name);
        s_apps[i].stop();

        /* 等最多 1s，让它的线程关掉 mic 设备 */
        for (int waited = 0; waited < 100; waited++)
        {
            if (!s_apps[i].is_running())
                break;
            rt_thread_mdelay(10);
        }
    }
}

int app_registry_start(int index)
{
    if (index < 0 || index >= APP_NUM)
        return -RT_EINVAL;
    if (s_apps[index].start == RT_NULL)
        return -RT_ENOSYS;

    if (s_apps[index].uses_mic)
        release_other_mic_users(index);

    return s_apps[index].start();
}

void app_registry_stop(int index)
{
    if (index < 0 || index >= APP_NUM)
        return;
    if (s_apps[index].stop != RT_NULL)
        s_apps[index].stop();
}
