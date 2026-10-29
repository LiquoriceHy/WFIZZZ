/*
 * ui_dashboard.c - 主页：大时钟 + 日期 + 温湿度/天气读数 + 当月日历
 *
 * 排版即层级：时钟最大，读数次之，标签最小最淡；读数不加卡片外框，
 * 仅用发丝分隔线分组（避免三个同构卡片把页面切成等重的块）。
 * 全页唯一的填充色块是日历中的"今天"，墨黑，作为视觉锚点。
 *
 * 时间来自 g_app.time_ts（app_data 已折算为本地秒）；RTC 未设置时显示
 * "--:--:--"。温湿度与天气走预留接口（app_data_set_*），未接入时显示占位。
 *
 * 日期换算不依赖 libc（工程未开 RT_USING_LIBC），使用 civil 历法算法
 *（Howard Hinnant，公有领域）在 epoch 秒与 y/m/d 之间转换。
 */
#include <rtthread.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "app_data.h"
#include "ui_memo.h"
#include "ui_alarm.h"

/* UTF-8 度数符号（montserrat 含 U+00B0 字形），显式转义避免源文件编码问题 */
#define DEG "\xC2\xB0"

/* ---------- 历法换算 ---------- */

static long days_from_civil(int y, int m, int d)
{
    long era;
    unsigned yoe, doy, doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097L + (long)doe - 719468L;
}

static void civil_from_days(long z, int *y, int *m, int *d)
{
    long era, yy;
    unsigned doe, yoe, doy, mp, dd, mm;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = (unsigned)(z - era * 146097);
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    yy = (long)yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp = (5 * doy + 2) / 153;
    dd = doy - (153 * mp + 2) / 5 + 1;
    mm = (mp < 10) ? mp + 3 : mp - 9;
    *y = (int)(yy + ((mm <= 2) ? 1 : 0));
    *m = (int)mm;
    *d = (int)dd;
}

static void ts_to_tm(rt_int64_t ts, int *y, int *mo, int *d,
                     int *h, int *mi, int *s, int *wd)
{
    long days = (long)(ts / 86400);
    int rem = (int)(ts % 86400);

    civil_from_days(days, y, mo, d);
    *h = rem / 3600;
    *mi = (rem % 3600) / 60;
    *s = rem % 60;
    /* 1970-01-01 是周四；0=周日 */
    *wd = (int)((days + 4) % 7);
    if (*wd < 0)
        *wd += 7;
}

static int days_in_month(int y, int m)
{
    static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0))
        return 29;
    return dm[m - 1];
}

/* ---------- 控件与布局 ---------- */

static const char *const MONTHS_CAP[12] =
{
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char *const WD_FULL[7] =
{
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const WD_CHARS[7] = {"S", "M", "T", "W", "T", "F", "S"};

/* 日历网格几何（白板 252 宽，内边距 10） */
#define CAL_GRID_W   232
#define CAL_CELL_W   33
#define CAL_CELL_H   40

/* 左栏读数区几何：三列等宽 + 两条发丝竖线分组 */
#define READ_COL_W   152
#define READ_GAP     10
#define READ_COL_X(i) ((i) * (READ_COL_W + READ_GAP))
#define READ_LABEL_Y 20      /* 读数区内部：小标签 */
#define READ_VALUE_Y 46      /* 读数区内部：大数值 */
#define READ_RULE_Y  14      /* 竖分隔线上下留白 */
#define READ_RULE_H  76

static lv_obj_t *clock_label;
static lv_obj_t *date_label;
static lv_obj_t *temp_value;
static lv_obj_t *hum_value;
static lv_obj_t *weather_value;
static lv_obj_t *cal_title;
static lv_obj_t *cal_grid;

static int cal_built_ymd = -1;   /* 已构建的 yyyymmdd，变了才重画 */

/* 发丝分隔线：1px 实心，不拦触摸 */
static lv_obj_t *hairline(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *line = lv_obj_create(parent);

    lv_obj_set_size(line, w, h);
    lv_obj_set_pos(line, x, y);
    lv_obj_set_style_bg_color(line, lv_color_hex(UI_SEP), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    return line;
}

static void calendar_rebuild(int y, int mo, int today)
{
    int first_wd = (int)((days_from_civil(y, mo, 1) + 4) % 7);
    int dim = days_in_month(y, mo);
    int d;

    lv_obj_clean(cal_grid);
    lv_label_set_text_fmt(cal_title, "%s %d", MONTHS_CAP[mo - 1], y);

    /* 星期头 */
    for (int i = 0; i < 7; i++)
    {
        lv_obj_t *wd = ui_label(cal_grid, &ui_style_small_dim);

        lv_label_set_text(wd, WD_CHARS[i]);
        lv_obj_set_width(wd, CAL_CELL_W);
        lv_obj_set_style_text_align(wd, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(wd, (lv_coord_t)(i * CAL_CELL_W), 2);
    }

    /* 日期格：今天用墨黑方块反白，是全页唯一的填充色块 */
    for (d = 1; d <= dim; d++)
    {
        int col = (first_wd + d - 1) % 7;
        int row = (first_wd + d - 1) / 7;
        lv_coord_t cy = (lv_coord_t)(30 + row * CAL_CELL_H);

        if (d == today)
        {
            lv_obj_t *hl = ui_card_dark_create(cal_grid, 24, 24);
            lv_obj_t *num = lv_label_create(hl);

            lv_obj_set_pos(hl, (lv_coord_t)(col * CAL_CELL_W + (CAL_CELL_W - 24) / 2),
                           (lv_coord_t)(cy + (CAL_CELL_H - 24) / 2));

            lv_label_set_text_fmt(num, "%d", d);
            lv_obj_set_style_text_font(num, UI_FONT_SMALL, 0);
            lv_obj_set_style_text_color(num, lv_color_hex(UI_CARD_BG), 0);
            lv_obj_center(num);
        }
        else
        {
            lv_obj_t *num = ui_label(cal_grid, &ui_style_small);

            lv_label_set_text_fmt(num, "%d", d);
            lv_obj_set_width(num, CAL_CELL_W);
            lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(num, (lv_coord_t)(col * CAL_CELL_W),
                           (lv_coord_t)(cy + (CAL_CELL_H - 20) / 2));
        }
    }
}

/* 读数：小标签 + 大数值，无外框（层级靠字号，不靠容器） */
static void reading(lv_obj_t *parent, int idx, const char *label,
                    lv_obj_t **value_out)
{
    lv_obj_t *t = ui_label(parent, &ui_style_small_dim);
    lv_obj_t *v = ui_label(parent, &ui_style_value);

    lv_label_set_text(t, label);
    lv_obj_set_pos(t, READ_COL_X(idx), READ_LABEL_Y);

    lv_label_set_text(v, "--");
    lv_obj_set_style_text_color(v, lv_color_hex(UI_OFFLINE), 0);
    lv_obj_set_pos(v, READ_COL_X(idx), READ_VALUE_Y);
    *value_out = v;
}

static void dash_timer_cb(lv_timer_t *timer)
{
    RT_UNUSED(timer);

    /* 时钟与日期（HH:MM 不显秒：时钟每分钟才变化，静态界面不应每秒触发
     * 全屏呈现，因为任何 dirty 都要付出全屏 VG-Lite 旋转的代价） */
    if (g_app.time_valid)
    {
        int y, mo, d, h, mi, s, wd;

        ts_to_tm(g_app.time_ts, &y, &mo, &d, &h, &mi, &s, &wd);
        ui_set_text_color_if_diff(clock_label, UI_INK);
        ui_label_set_text_fmt_if_diff(clock_label, "%02d:%02d", h, mi);
        ui_set_text_color_if_diff(date_label, UI_TEXT_DIM);
        ui_label_set_text_fmt_if_diff(date_label, "%s, %s %d, %d",
                                      WD_FULL[wd], MONTHS_CAP[mo - 1], d, y);

        /* 日历：日期变了才重画 */
        if (cal_built_ymd != y * 10000 + mo * 100 + d)
        {
            calendar_rebuild(y, mo, d);
            cal_built_ymd = y * 10000 + mo * 100 + d;
        }
    }
    else
    {
        ui_set_text_color_if_diff(clock_label, UI_OFFLINE);
        ui_label_set_text_if_diff(clock_label, "--:--");
        ui_set_text_color_if_diff(date_label, UI_OFFLINE);
        ui_label_set_text_if_diff(date_label, "Time not set  (msh: date)");
    }

    /* 温湿度：M33 AHT20 经 IPC 帧接入，30s 无帧回落 "--" */
    if (g_app.temp_hum_valid)
    {
        int t10 = (int)(g_app.temp_c * 10.0f + (g_app.temp_c < 0 ? -0.5f : 0.5f));
        int dec = t10 % 10;

        if (dec < 0)
            dec = -dec;
        ui_label_set_text_fmt_if_diff(temp_value, "%d.%d" DEG "C", t10 / 10, dec);
        ui_set_text_color_if_diff(temp_value, UI_INK);
        ui_label_set_text_fmt_if_diff(hum_value, "%d %%", (int)(g_app.hum_pct + 0.5f));
        ui_set_text_color_if_diff(hum_value, UI_INK);
    }
    else
    {
        ui_label_set_text_if_diff(temp_value, "--");
        ui_set_text_color_if_diff(temp_value, UI_OFFLINE);
        ui_label_set_text_if_diff(hum_value, "--");
        ui_set_text_color_if_diff(hum_value, UI_OFFLINE);
    }

    /* 天气（预留读数） */
    if (g_app.weather_text[0] != '\0')
    {
        ui_label_set_text_if_diff(weather_value, (const char *)g_app.weather_text);
        ui_set_text_color_if_diff(weather_value, UI_INK);
    }
    else
    {
        ui_label_set_text_if_diff(weather_value, "TBD");
        ui_set_text_color_if_diff(weather_value, UI_OFFLINE);
    }
}

lv_obj_t *ui_dashboard_create(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_t *head;      /* 时钟 + 日期：纵向 flex，随字号自动排布 */
    lv_obj_t *reads;     /* 三列读数：裸排版 + 发丝线分组 */
    lv_obj_t *cal;

    lv_obj_set_size(page, UI_SCREEN_W, UI_PAGE_H);
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    /* 时钟 + 日期（左上，页面主标题） */
    head = lv_obj_create(page);
    lv_obj_set_size(head, 476, 110);
    lv_obj_set_pos(head, UI_PAD, 16);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(head, 0, 0);
    lv_obj_set_style_pad_row(head, 8, 0);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_radius(head, 0, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    clock_label = lv_label_create(head);
    lv_obj_set_style_text_font(clock_label, UI_FONT_CLOCK, 0);
    lv_obj_set_style_text_color(clock_label, lv_color_hex(UI_OFFLINE), 0);
    lv_label_set_text(clock_label, "--:--:--");

    date_label = ui_label(head, &ui_style_dim);
    lv_label_set_text(date_label, "Time not set");

    /* 读数区：顶部发丝线 + 两条竖线分组，无卡片外框 */
    reads = lv_obj_create(page);
    lv_obj_set_size(reads, 476, 110);
    lv_obj_set_pos(reads, UI_PAD, 150);
    lv_obj_set_style_bg_opa(reads, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(reads, 0, 0);
    lv_obj_set_style_radius(reads, 0, 0);
    lv_obj_set_style_pad_all(reads, 0, 0);
    lv_obj_clear_flag(reads, LV_OBJ_FLAG_SCROLLABLE);

    hairline(reads, 0, 0, 476, 1);
    hairline(reads, READ_COL_X(0) + READ_COL_W + READ_GAP / 2, READ_RULE_Y, 1, READ_RULE_H);
    hairline(reads, READ_COL_X(1) + READ_COL_W + READ_GAP / 2, READ_RULE_Y, 1, READ_RULE_H);

    reading(reads, 0, "Temperature", &temp_value);
    reading(reads, 1, "Humidity", &hum_value);
    reading(reads, 2, "Weather", &weather_value);

    /* 日历（右侧）：页面唯一容器，墨黑"今天"是唯一填充色块 */
    cal = ui_card_create(page, 252, 412);
    lv_obj_set_pos(cal, 524, 16);

    /* 时间未就绪时保持中性标题，避免显示一个错误的月份 */
    cal_title = ui_label(cal, &ui_style_text);
    lv_label_set_text(cal_title, "Calendar");
    lv_obj_set_pos(cal_title, 10, 10);

    cal_grid = lv_obj_create(cal);
    lv_obj_set_size(cal_grid, CAL_GRID_W, 350);
    lv_obj_set_pos(cal_grid, 10, 42);
    lv_obj_set_style_bg_opa(cal_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cal_grid, 0, 0);
    lv_obj_set_style_radius(cal_grid, 0, 0);
    lv_obj_set_style_pad_all(cal_grid, 0, 0);
    lv_obj_clear_flag(cal_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(cal_grid, LV_SCROLLBAR_MODE_OFF);

    /* 墨墨卡片：左栏底部 168x168，y=读数区下缘（150+110），底边与日历对齐 */
    ui_memo_create(page, UI_PAD, 246);

    /* 闹钟卡片：位于墨墨卡片右侧 292x182，右缘与读数区（24+476=500）对齐。
     * y 从 260 调整为 246：卡片加高到 182 后（增加便签两行），260+182=442
     * 会贴近页面底边（444）。改为 246 后底边为 428，保留 16px 余量。
     * 墨墨卡片同步上移以保持两卡顶边对齐，其自身尺寸不变。 */
    ui_alarm_card_create(page, UI_PAD + 168 + 16, 246);

    lv_timer_create(dash_timer_cb, 1000, NULL);
    dash_timer_cb(NULL);

    return page;
}
