/*
 * ui_alarm.c - 闹钟 UI：主页卡片 + 二级设置界面 + 响铃遮罩
 *
 * 数据全部经 g_app（volatile）与 alarm 线程交互：卡片与列表读取
 * g_app.alarms，编辑调用 app_data_set_alarm()/app_data_set_alarm_ring()，
 * UI 内不保留状态副本，因为 alarm 线程是触发方，g_app 为唯一数据源。
 * 风格沿用 ui_theme：暖白底、墨黑锚点、全直角，橙色仅用于"需要关注"
 * （响铃或时间未设置）。
 */
#include <rtthread.h>
#include <stdint.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "app_data.h"
#include "alarm.h"
#include "note.h"
#include "ui_alarm.h"
#include "screen_saver.h"

/* ---------- 小工具 ---------- */

/* 周几缩写，下标 = days 位序（bit0=Mon..bit6=Sun） */
static const char *const DAY_ABBR[7] =
{ "Mo", "Tu", "We", "Th", "Fr", "Sa", "Su" };

/* days 位掩码转换为展示文案：0=Once、全选=Daily、Mon-Fri 连续位=Mon-Fri，
 * 其余逐日缩写拼接（如 "Mo,We,Fr"） */
static void alarm_days_text(rt_uint8_t days, char *buf, int len)
{
    int pos = 0;
    int b;

    if (days == 0)
    {
        rt_snprintf(buf, len, "Once");
        return;
    }
    if (days == APP_ALARM_DAYS_ALL)
    {
        rt_snprintf(buf, len, "Daily");
        return;
    }
    if (days == APP_ALARM_DAYS_WEEKDAYS)
    {
        rt_snprintf(buf, len, "Mon-Fri");
        return;
    }

    for (b = 0; b < 7 && pos < len - 4; b++)
    {
        if (days & (rt_uint8_t)(1u << b))
        {
            if (pos > 0)
                buf[pos++] = ',';
            buf[pos++] = DAY_ABBR[b][0];
            buf[pos++] = DAY_ABBR[b][1];
        }
    }
    buf[pos] = '\0';
}

/* 行/卡片内墨黑按钮（白字）与白底按钮（描边）工厂 */
static lv_obj_t *btn_make(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h, const char *txt,
                          int dark, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lab = lv_label_create(btn);

    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 0, 0);
    if (dark)
    {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_border_width(btn, 0, 0);
    }
    else
    {
        lv_obj_set_style_bg_color(btn, lv_color_hex(UI_CARD_BG), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(UI_CARD_EDGE), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
    }
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_label_set_text(lab, txt);
    lv_obj_set_style_text_color(lab, lv_color_hex(dark ? UI_CARD_BG : UI_INK), 0);
    lv_obj_set_style_text_font(lab, UI_FONT_TEXT, 0);
    lv_obj_center(lab);

    if (cb)
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    return btn;
}

/* ---------- 主页卡片：下一个将触发的闹钟摘要 ---------- */

static void open_card_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    ui_alarm_open();   /* ui_alarm.h 已声明，此处仅转跳 */
}

/* ---------- 主页卡片：下一个闹钟 + 便签摘要 ---------- */

/* 卡片从 168 加高到 182：下半部分要给便签区腾地方。
 * 168 的内部布局是 flex column（title 16 + value 24 + note 16 + 两处
 * pad_row 10 = 86），下方约 50px 空着；加了发丝线 + "Note n/4" +
 * 两条便签（各 16px）后需要约 180。 */
#define AL_CARD_W    292
#define AL_CARD_H    182
#define AL_CARD_PAD  16

static lv_obj_t *card_value;
static lv_obj_t *card_note;
static lv_obj_t *card_note_head;    /* "Note 2/4" */
static lv_obj_t *card_note_line[2]; /* 便签摘要，最多两行 */

/* 距下一次触发的分钟数（-1=未来 7 天内不触发；周几规则参与推算） */
static int alarm_next_in(rt_int64_t ts, const app_data_alarm_t *a)
{
    int now_wd = (int)(((ts / 86400) + 4) % 7);
    int now_min = (int)((ts / 60) % 1440);
    int d;

    if (now_wd < 0)
        now_wd += 7;

    for (d = 0; d < 7; d++)
    {
        int wd = (now_wd + d) % 7;   /* 0=Sun..6=Sat */
        int t;

        if (a->days != 0 && !(a->days & APP_ALARM_DAY_BIT(wd)))
            continue;

        t = d * 1440 + (int)a->hour * 60 + (int)a->min - now_min;
        if (t > 0)
            return t;
    }
    return -1;
}

/* 刷新便签摘要区：标题行 "Note n/4" + 最多两条未完成的便签。
 *
 * 只显示未完成的：已勾掉的条目对"今天还要做什么"没有信息量。
 * 全都完成（或没有便签）时显示 "(empty)"，避免卡片下半部分空白一片
 * 让人以为是渲染坏了。 */
static void card_refresh_notes(void)
{
    char head[16];
    char texts[NOTE_MAX][NOTE_TEXT_LEN];
    rt_bool_t dones[NOTE_MAX];
    int i, shown = 0, total = 0;
    int pending = 0;

    /* 先把 4 条全拷出来再统计与显示：note_get_copy 每条都进一次临界区，
     * 而后面还要用同一份数据算 pending 和取前两条，分开取会在两次读之间
     * 被 UI/AI 改动，出现"标题说 2 条但只列了 1 条"的不一致。 */
    for (i = 0; i < NOTE_MAX; i++)
    {
        dones[i] = RT_FALSE;
        texts[i][0] = '\0';

        if (!note_get_copy(i, texts[i], NOTE_TEXT_LEN, &dones[i]))
            continue;

        total++;
        if (!dones[i]) pending++;
    }

    rt_snprintf(head, sizeof(head), "Note %d/%d", pending, NOTE_MAX);
    ui_label_set_text_if_diff(card_note_head, head);
    ui_set_text_color_if_diff(card_note_head,
                              pending > 0 ? UI_INK : UI_MUTED);

    /* 取前两条未完成的填进两行 */
    for (i = 0; i < NOTE_MAX && shown < 2; i++)
    {
        char line[NOTE_TEXT_LEN + 8];

        if (texts[i][0] == '\0' || dones[i]) continue;

        /* 前缀用 ASCII 短横 + 空格。不用 LV_SYMBOL_BULLET：
         * 那个符号在 montserrat_16 里有点，但和中文正文混排时基线偏移，
         * 而中文正文本来就是空白（本期无中文字库），反而更难对齐。 */
        rt_snprintf(line, sizeof(line), "- %s", texts[i]);
        ui_label_set_text_if_diff(card_note_line[shown], line);
        shown++;
    }

    for (; shown < 2; shown++)
    {
        /* 空行填 "-"：保持两行控件高度一致，避免卡片高度随内容跳动 */
        ui_label_set_text_if_diff(card_note_line[shown],
                                  (total == 0 && shown == 0) ? "(empty)" : "-");
    }
}

static void card_timer_cb(lv_timer_t *timer)
{
    rt_int64_t ts = g_app.time_ts;
    int i, best = -1;
    int best_in = 0;

    RT_UNUSED(timer);

    if (g_app.time_valid)
    {
        for (i = 0; i < APP_ALARM_NUM; i++)
        {
            app_data_alarm_t a = g_app.alarms[i];
            int in;

            if (!a.enable)
                continue;
            in = alarm_next_in(ts, &a);
            if (in < 0)
                continue;
            if (best < 0 || in < best_in)
            {
                best = i;
                best_in = in;
            }
        }
    }

    if (best >= 0)
    {
        app_data_alarm_t a = g_app.alarms[best];
        char txt[24];

        ui_label_set_text_fmt_if_diff(card_value, "%02d:%02d", (int)a.hour, (int)a.min);
        ui_set_text_color_if_diff(card_value, UI_INK);
        alarm_days_text(a.days, txt, sizeof(txt));
        ui_label_set_text_if_diff(card_note, txt);
        ui_set_text_color_if_diff(card_note, UI_TEXT_DIM);
    }
    else if (!g_app.time_valid)
    {
        ui_label_set_text_if_diff(card_value, "--");
        ui_set_text_color_if_diff(card_value, UI_OFFLINE);
        ui_label_set_text_if_diff(card_note, "no time");
        ui_set_text_color_if_diff(card_note, UI_ATTENTION);
    }
    else
    {
        ui_label_set_text_if_diff(card_value, "--");
        ui_set_text_color_if_diff(card_value, UI_OFFLINE);
        ui_label_set_text_if_diff(card_note, "Off");
        ui_set_text_color_if_diff(card_note, UI_TEXT_DIM);
    }

    /* 便签区与闹钟区一起刷：同一个 1 秒 timer，不新开 timer。
     * LVGL 的每个 timer 都要在每次 tick 里被调度，多开一个就是多一份
     * 常数开销；而这两块数据的更新频率需求完全一样。 */
    card_refresh_notes();
}

lv_obj_t *ui_alarm_card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *card = ui_card_create(parent, AL_CARD_W, AL_CARD_H);
    lv_obj_t *title;
    lv_obj_t *sep;
    int i;

    lv_obj_set_pos(card, x, y);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(card, AL_CARD_PAD, 0);
    /* 行距从 10 收到 4：加高后的卡片要塞 7 行内容，10px 行距会把
     * 便签两行挤到卡片外（flex 不会报错，只是被裁掉）。 */
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, open_card_cb, LV_EVENT_CLICKED, NULL);

    title = ui_label(card, &ui_style_small_dim);
    lv_label_set_text(title, "Alarm");

    card_value = ui_label(card, &ui_style_value);
    lv_label_set_text(card_value, "--");

    card_note = ui_label(card, &ui_style_small_dim);
    lv_label_set_text(card_note, "Off");

    /* 发丝线：分割"闹钟"与"便签"两个语义区。宽度用百分比，
     * 卡片宽度以后调整也不用改这里。 */
    sep = lv_obj_create(card);
    lv_obj_set_size(sep, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(sep, lv_color_hex(UI_SEP), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE);

    card_note_head = ui_label(card, &ui_style_small_dim);
    lv_label_set_text(card_note_head, "Note 0/4");

    for (i = 0; i < 2; i++)
    {
        /* 便签正文用 TEXT 样式而不是 small：small 固定是纯 ASCII 的
         * montserrat_14，汉字一个字都渲染不出来；TEXT 在字库加载后指向
         * cjk16（含 GB2312），且其 fallback 是 montserrat_16，ASCII 便签
         * 也不会变空白。AI 写入的便签基本是中文，这条是硬需求。
         *
         * 代价是这两行比卡片其它文字大 2px。便签是卡片里唯一的内容型
         * 信息，略大反而更像正文，可接受。 */
        card_note_line[i] = ui_label(card, &ui_style_text);
        lv_label_set_text(card_note_line[i], "-");
        lv_label_set_long_mode(card_note_line[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(card_note_line[i], AL_CARD_W - AL_CARD_PAD * 2);
    }

    lv_timer_create(card_timer_cb, 1000, NULL);
    card_timer_cb(NULL);

    return card;
}

/* ---------- 二级设置界面：列表态 + 编辑态 ---------- */

static lv_obj_t *ov;            /* 全屏覆盖层（懒创建，Back 隐藏） */
static lv_obj_t *list_view;
static lv_obj_t *edit_view;
static lv_obj_t *note_view;     /* 便签态（第三态，与上两者互斥） */
static lv_obj_t *ov_note;       /* 头部提示：时间未设(橙)/点击行编辑(灰) */
static lv_obj_t *row_time[APP_ALARM_NUM];
static lv_obj_t *row_rep[APP_ALARM_NUM];
static lv_obj_t *row_sw[APP_ALARM_NUM];   /* 开关：AI 改 enable 时要同步视觉状态 */

/* ---- 便签态控件：4 行固定，一次创建永不重建（与闹钟列表同一套纪律）---- */
static lv_obj_t *nt_row[NOTE_MAX];       /* 行容器（可点击=勾选完成） */
static lv_obj_t *nt_check[NOTE_MAX];     /* "[x]" / "[ ]" */
static lv_obj_t *nt_text[NOTE_MAX];      /* 正文 */
static lv_obj_t *nt_empty;               /* "No notes" 提示，全空时显示 */

/* 列表固定 APP_ALARM_NUM 行、一次创建永不重建：行内容全部就地刷新，
 * 从根本上避免"事件回调里 lv_obj_clean 删对象"的不安全旧模式 */

static int edit_idx;            /* 编辑态当前槽位 */
static int edit_h, edit_m;      /* 编辑态时分缓存（箭头步进改这里） */
static rt_uint8_t edit_days;    /* 编辑态周几掩码（pill 切换改这里） */
static lv_obj_t *num_h_lbl;
static lv_obj_t *num_m_lbl;
static lv_obj_t *day_btn[7];
static lv_obj_t *day_lbl[7];
static lv_obj_t *days_hint;     /* 组合语义行："Once"/"Mon-Fri"/… */

static void edit_num_refresh(void);
static void days_pill_refresh(void);

/* 全直角开关：轨道/指示器/旋钮全部去圆角并按主题上色 */
static lv_obj_t *switch_make(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             int checked, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *sw = lv_switch_create(parent);

    lv_obj_set_size(sw, 64, 40);
    lv_obj_set_pos(sw, x, y);
    lv_obj_set_style_radius(sw, 0, 0);
    lv_obj_set_style_radius(sw, 0, LV_PART_INDICATOR);
    lv_obj_set_style_radius(sw, 0, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_MUTED), 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_MUTED), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_INK),
                              LV_STATE_CHECKED | LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, lv_color_hex(UI_CARD_BG), LV_PART_KNOB);

    if (checked)
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb)
        lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    return sw;
}

static void row_refresh(int i)
{
    app_data_alarm_t a = g_app.alarms[i];
    char txt[24];

    ui_label_set_text_fmt_if_diff(row_time[i], "%02d:%02d",
                                  (int)a.hour, (int)a.min);
    ui_set_text_color_if_diff(row_time[i], a.enable ? UI_INK : UI_MUTED);
    alarm_days_text(a.days, txt, sizeof(txt));
    ui_label_set_text_if_diff(row_rep[i], txt);
    ui_set_text_color_if_diff(row_rep[i], a.enable ? UI_TEXT_DIM : UI_MUTED);

    /* 同步开关视觉状态。
     *
     * 必须做：AI 通过 MCP 的 self.alarm.set/clear 改 enable 时不会触发
     * switch 的事件回调（那是用户点击才走的路径），列表上的开关会一直
     * 停在旧位置，出现"AI 说关了但开关还是开的"这种自相矛盾的界面。
     * list_refresh_all() 在每次打开二级界面时都会调到这里。 */
    if (row_sw[i] != RT_NULL)
    {
        if (a.enable)
            lv_obj_add_state(row_sw[i], LV_STATE_CHECKED);
        else
            lv_obj_remove_state(row_sw[i], LV_STATE_CHECKED);
    }
}

static void switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    app_data_alarm_t a = g_app.alarms[i];

    a.enable = lv_obj_has_state(sw, LV_STATE_CHECKED) ? RT_TRUE : RT_FALSE;
    app_data_set_alarm(i, &a);
    app_alarm_save_request();   /* 开关状态也要落盘，否则重启后回到旧值 */
    row_refresh(i);   /* 就地改色，不重建列表（事件分发中删对象不安全） */
}

/* 进编辑态：把槽位数据拷进编辑缓存，控件全部就地刷新 */
static void edit_enter(int i)
{
    edit_idx  = i;
    edit_h    = g_app.alarms[i].hour;
    edit_m    = g_app.alarms[i].min;
    edit_days = g_app.alarms[i].days;

    edit_num_refresh();
    days_pill_refresh();

    lv_obj_add_flag(list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(edit_view, LV_OBJ_FLAG_HIDDEN);
}

static void row_click_cb(lv_event_t *e)
{
    edit_enter((int)(intptr_t)lv_event_get_user_data(e));
}

/* 列表一次创建（ov_build 后调用一次），内容刷新走 row_refresh */
static void list_create(void)
{
    int i;

    for (i = 0; i < APP_ALARM_NUM; i++)
    {
        lv_obj_t *row = ui_card_create(list_view, 752, 104);
        lv_obj_t *lab;

        /* 行的纵位由 list_view 的 flex 列布局决定（行高 104 + 间距 12） */
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        row_time[i] = ui_label(row, &ui_style_value);
        lv_obj_set_pos(row_time[i], 20, 32);

        row_rep[i] = ui_label(row, &ui_style_small_dim);
        lv_obj_set_pos(row_rep[i], 180, 42);

        row_sw[i] = switch_make(row, 664, 32, g_app.alarms[i].enable ? 1 : 0,
                                switch_cb, (void *)(intptr_t)i);

        lab = ui_label(row, &ui_style_small_dim);
        lv_label_set_text(lab, "tap to edit");
        lv_obj_set_pos(lab, 180, 62);
        lv_obj_set_style_text_color(lab, lv_color_hex(UI_MUTED), 0);

        row_refresh(i);
    }
}

static void list_refresh_all(void)
{
    int i;

    for (i = 0; i < APP_ALARM_NUM; i++)
        row_refresh(i);
}

static void edit_num_refresh(void)
{
    ui_label_set_text_fmt_if_diff(num_h_lbl, "%02d", edit_h);
    ui_label_set_text_fmt_if_diff(num_m_lbl, "%02d", edit_m);
}

static void days_pill_refresh(void)
{
    char txt[24];
    int b;

    for (b = 0; b < 7; b++)
    {
        int on = (edit_days >> b) & 1;

        lv_obj_set_style_bg_color(day_btn[b],
            lv_color_hex(on ? UI_INK : UI_CARD_BG), 0);
        lv_obj_set_style_border_width(day_btn[b], on ? 0 : 1, 0);
        lv_obj_set_style_text_color(day_lbl[b],
            lv_color_hex(on ? UI_CARD_BG : UI_INK), 0);
    }

    alarm_days_text(edit_days, txt, sizeof(txt));
    ui_label_set_text_if_diff(days_hint, txt);
}

/* 箭头步进：单击 ±1、长按连发（LVGL 长按后松手不发 CLICKED，不叠加）。
 * code: 0=时+ 1=时- 2=分+ 3=分-；+25/%24 之类的写法实现 23/59 环绕 */
static void step_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e);

    switch (code)
    {
    case 0:  edit_h = (edit_h + 25) % 24; break;
    case 1:  edit_h = (edit_h + 23) % 24; break;
    case 2:  edit_m = (edit_m + 61) % 60; break;
    default: edit_m = (edit_m + 59) % 60; break;
    }
    edit_num_refresh();
}

/* 周几 pill：点击翻转该天，全不选 = Once（单次） */
static void day_cb(lv_event_t *e)
{
    int b = (int)(intptr_t)lv_event_get_user_data(e);

    edit_days ^= (rt_uint8_t)(1u << b);
    days_pill_refresh();
}

static void edit_save_cb(lv_event_t *e)
{
    app_data_alarm_t a;

    RT_UNUSED(e);

    /* 先整体拷旧值再覆盖：原来这里只赋 4 个字段就传给 setter，恰好
     * 这 4 个就是结构体全部字段所以暂时安全，但以后 app_data_alarm_t
     * 一旦加字段（比如 label），这里就会把未初始化内存写进去。 */
    a = g_app.alarms[edit_idx];
    a.hour   = (rt_uint8_t)edit_h;
    a.min    = (rt_uint8_t)edit_m;
    a.days   = edit_days;
    /* enable 保持原值：开关在列表态改，保存不改 */

    app_data_set_alarm(edit_idx, &a);
    app_alarm_invalidate_slot(edit_idx);        /* 改成当前分钟也能触发 */
    app_alarm_save_request();                   /* 置脏，1 秒线程落盘 */

    lv_obj_add_flag(edit_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_view, LV_OBJ_FLAG_HIDDEN);
    row_refresh(edit_idx);   /* 就地刷新该行，不重建列表 */
}

/* 返回按钮：便签态和编辑态都先回列表（编辑态丢弃未保存修改），
 * 列表态关闭整个二级界面 */
static void go_back(void)
{
    if (!lv_obj_has_flag(note_view, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_add_flag(note_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(list_view, LV_OBJ_FLAG_HIDDEN);
    }
    else if (!lv_obj_has_flag(edit_view, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_add_flag(edit_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(list_view, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);
    }
}

static void edit_cancel_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    go_back();
}

static void back_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    go_back();
}

/* ---------- 便签态：4 行待办 + Clear Done ---------- */

/* 刷新一行便签的外观。空槽整行隐藏，保持列表紧凑。 */
static void note_row_refresh(int idx)
{
    char text[NOTE_TEXT_LEN];
    rt_bool_t done = RT_FALSE;

    if (!note_get_copy(idx, text, sizeof(text), &done))
    {
        lv_obj_add_flag(nt_row[idx], LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(nt_row[idx], LV_OBJ_FLAG_HIDDEN);
    ui_label_set_text_if_diff(nt_check[idx], done ? "[x]" : "[ ]");
    ui_set_text_color_if_diff(nt_check[idx], done ? UI_MUTED : UI_INK);
    ui_label_set_text_if_diff(nt_text[idx], text);
    ui_set_text_color_if_diff(nt_text[idx], done ? UI_MUTED : UI_INK);
}

/* 每行旁边的小删除按钮：按 idx 直接清空该槽（note_set 空串等价删除）。 */
static void note_del_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    note_del(idx);

    /* 删完要把整列重刷：note_del 会把后续条目前移，只刷本行会留下残留。 */
    for (int i = 0; i < NOTE_MAX; i++)
        note_row_refresh(i);

    ui_label_set_text_if_diff(nt_empty,
                              note_count() == 0 ? "No notes. Ask AI to add one."
                                                : "");
}

/* 点击行体 = 切换完成状态 */
static void note_toggle_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    char text[NOTE_TEXT_LEN];
    rt_bool_t done = RT_FALSE;

    if (!note_get_copy(idx, text, sizeof(text), &done))
        return;

    RT_UNUSED(text);   /* 只关心完成状态，文本由 note_row_refresh 重新取 */

    note_mark(idx, done ? RT_FALSE : RT_TRUE);
    note_row_refresh(idx);
}

static void note_clear_done_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    note_clear_done();
    for (int i = 0; i < NOTE_MAX; i++)
        note_row_refresh(i);

    ui_label_set_text_if_diff(nt_empty,
                              note_count() == 0 ? "No notes. Ask AI to add one."
                                                : "");
}

static void note_open_cb(lv_event_t *e)
{
    int i;

    RT_UNUSED(e);

    for (i = 0; i < NOTE_MAX; i++)
        note_row_refresh(i);

    ui_label_set_text_if_diff(nt_empty,
                              note_count() == 0 ? "No notes. Ask AI to add one."
                                                : "");

    /* 便签态提示行：说明入口在语音，因为屏上没有中文输入法
     * （lv_keyboard 只有 ASCII 模式，见 ui_wifi.c 的用法）。 */
    ui_label_set_text_if_diff(ov_note, "Ask AI to add notes. Tap to toggle, X to delete.");
    ui_set_text_color_if_diff(ov_note, UI_TEXT_DIM);

    lv_obj_add_flag(list_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(edit_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(note_view, LV_OBJ_FLAG_HIDDEN);
}

/* 建便签态容器。与 list_view/edit_view 同位置同尺寸、三态互斥。 */
static void note_view_create(void)
{
    note_view = lv_obj_create(ov);
    lv_obj_set_size(note_view, 752, 348);
    lv_obj_set_pos(note_view, UI_PAD, 76);
    lv_obj_set_style_bg_opa(note_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(note_view, 0, 0);
    lv_obj_set_style_pad_all(note_view, 0, 0);
    lv_obj_set_flex_flow(note_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(note_view, 8, 0);
    lv_obj_set_scrollbar_mode(note_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(note_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(note_view, LV_OBJ_FLAG_HIDDEN);

    nt_empty = ui_label(note_view, &ui_style_small_dim);
    lv_label_set_text(nt_empty, "");
    lv_obj_set_pos(nt_empty, 4, 4);

    for (int i = 0; i < NOTE_MAX; i++)
    {
        lv_obj_t *del;

        /* 行高 52：4 行 × 52 + 3 × 8 间距 = 232，加上 Clear 按钮
         * 和提示区，348 的高度足够，还留有余量给将来加到 6 条。 */
        nt_row[i] = ui_card_create(note_view, 752, 52);
        lv_obj_add_flag(nt_row[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(nt_row[i], note_toggle_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        nt_check[i] = ui_label(nt_row[i], &ui_style_text);
        lv_label_set_text(nt_check[i], "[ ]");
        lv_obj_set_pos(nt_check[i], 16, 14);

        nt_text[i] = ui_label(nt_row[i], &ui_style_text);
        lv_label_set_text(nt_text[i], "");
        lv_obj_set_pos(nt_text[i], 70, 14);
        lv_obj_set_width(nt_text[i], 600);
        lv_label_set_long_mode(nt_text[i], LV_LABEL_LONG_DOT);

        del = btn_make(nt_row[i], 690, 8, 44, 36, "X", 0,
                       note_del_cb, (void *)(intptr_t)i);
        RT_UNUSED(del);

        lv_obj_add_flag(nt_row[i], LV_OBJ_FLAG_HIDDEN);
    }

    btn_make(note_view, 0, 264, 200, 44, "Clear Done", 0,
             note_clear_done_cb, NULL);
}

static void ov_build(void)
{
    lv_obj_t *title;

    ov = lv_obj_create(lv_scr_act());
    lv_obj_set_size(ov, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(ov, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_set_style_pad_all(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_HIDDEN);

    /* 头部：Back（左上，三态共用）+ 标题 + 提示行 + Note（右上，进便签态）
     *
     * 为什么便签入口在这里而不是加第四个闹钟行：list_view 已经被
     * 3 行 × 104 塞满（336/348），再挤一行必然溢出且不滚动。头部右侧
     * 那块一直空着，把便签做成独立态在空间上零冲突。 */
    btn_make(ov, UI_PAD, 12, 120, 44, "Back", 1, back_cb, NULL);

    title = ui_label(ov, &ui_style_text);
    lv_label_set_text(title, "Alarms");
    lv_obj_set_pos(title, 160, 22);

    ov_note = ui_label(ov, &ui_style_small_dim);
    lv_obj_set_pos(ov_note, 160, 50);

    btn_make(ov, UI_SCREEN_W - UI_PAD - 120, 12, 120, 44, "Note", 0,
             note_open_cb, NULL);

    /* 列表态容器：行由 list_create() 一次建满 */
    list_view = lv_obj_create(ov);
    lv_obj_set_size(list_view, 752, 348);
    lv_obj_set_pos(list_view, UI_PAD, 76);
    lv_obj_set_flex_flow(list_view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list_view, 0, 0);
    lv_obj_set_style_pad_row(list_view, 12, 0);
    lv_obj_set_style_bg_opa(list_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_view, 0, 0);
    lv_obj_set_scrollbar_mode(list_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(list_view, LV_OBJ_FLAG_SCROLLABLE);

    /* 编辑态容器 */
    edit_view = lv_obj_create(ov);
    lv_obj_set_size(edit_view, 752, 348);
    lv_obj_set_pos(edit_view, UI_PAD, 76);
    lv_obj_set_style_bg_opa(edit_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(edit_view, 0, 0);
    lv_obj_set_style_pad_all(edit_view, 0, 0);
    lv_obj_set_scrollbar_mode(edit_view, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(edit_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(edit_view, LV_OBJ_FLAG_HIDDEN);

    /* 便签态容器（第三态）：与上面两者同位置同尺寸，互斥显隐 */
    note_view_create();
}

/* 编辑态面板：大数字 + 上下箭头 + 7 个周几 pill + Cancel/Save。
 * （替换旧双 roller：60 项拖动慢且容易过冲；箭头单击/长按连发更直接） */
static void edit_create(void)
{
    static const char day_ch[7] = { 'M', 'T', 'W', 'T', 'F', 'S', 'S' };
    lv_obj_t *lab;
    lv_obj_t *btn;
    int b;

    /* 时栏（x 222..318）/ 分栏（x 434..530）：▲ 数字 ▼ 三段 */
    btn = btn_make(edit_view, 222, 4, 96, 44, LV_SYMBOL_UP, 0,
                   step_cb, (void *)(intptr_t)0);
    lv_obj_add_event_cb(btn, step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *)(intptr_t)0);

    btn = btn_make(edit_view, 222, 116, 96, 44, LV_SYMBOL_DOWN, 0,
                   step_cb, (void *)(intptr_t)1);
    lv_obj_add_event_cb(btn, step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *)(intptr_t)1);

    num_h_lbl = lv_label_create(edit_view);
    lv_obj_set_style_text_font(num_h_lbl, UI_FONT_CLOCK, 0);
    lv_obj_set_style_text_color(num_h_lbl, lv_color_hex(UI_INK), 0);
    lv_obj_set_width(num_h_lbl, 96);
    lv_obj_set_style_text_align(num_h_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(num_h_lbl, 222, 60);
    lv_label_set_text(num_h_lbl, "00");

    lab = lv_label_create(edit_view);
    lv_obj_set_style_text_font(lab, UI_FONT_CLOCK, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(UI_TEXT_DIM), 0);
    lv_label_set_text(lab, ":");
    lv_obj_set_pos(lab, 366, 62);

    btn = btn_make(edit_view, 434, 4, 96, 44, LV_SYMBOL_UP, 0,
                   step_cb, (void *)(intptr_t)2);
    lv_obj_add_event_cb(btn, step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *)(intptr_t)2);

    btn = btn_make(edit_view, 434, 116, 96, 44, LV_SYMBOL_DOWN, 0,
                   step_cb, (void *)(intptr_t)3);
    lv_obj_add_event_cb(btn, step_cb, LV_EVENT_LONG_PRESSED_REPEAT,
                        (void *)(intptr_t)3);

    num_m_lbl = lv_label_create(edit_view);
    lv_obj_set_style_text_font(num_m_lbl, UI_FONT_CLOCK, 0);
    lv_obj_set_style_text_color(num_m_lbl, lv_color_hex(UI_INK), 0);
    lv_obj_set_width(num_m_lbl, 96);
    lv_obj_set_style_text_align(num_m_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(num_m_lbl, 434, 60);
    lv_label_set_text(num_m_lbl, "00");

    /* 周几 pill（周一开头，bit 序同 DAY_ABBR）：多选，全不选=Once */
    lab = ui_label(edit_view, &ui_style_small_dim);
    lv_label_set_text(lab, "Repeat");
    lv_obj_set_pos(lab, 0, 200);

    for (b = 0; b < 7; b++)
    {
        char s[2] = { day_ch[b], '\0' };

        day_btn[b] = btn_make(edit_view, (lv_coord_t)(110 + b * 72), 186,
                              64, 44, s, 0, day_cb, (void *)(intptr_t)b);
        day_lbl[b] = lv_obj_get_child(day_btn[b], 0);
    }

    days_hint = ui_label(edit_view, &ui_style_small_dim);
    lv_obj_set_pos(days_hint, 110, 244);

    btn_make(edit_view, 60, 292, 160, 44, "Cancel", 0, edit_cancel_cb, NULL);
    btn_make(edit_view, 532, 292, 160, 44, "Save", 1, edit_save_cb, NULL);
}

void ui_alarm_open(void)
{
    if (ov == RT_NULL)
    {
        ov_build();
        list_create();
        edit_create();
    }

    if (!g_app.time_valid)
    {
        ui_label_set_text_if_diff(ov_note, "Time not set - connect WiFi for NTP");
        ui_set_text_color_if_diff(ov_note, UI_ATTENTION);
    }
    else
    {
        ui_label_set_text_if_diff(ov_note, "Tap an alarm to edit");
        ui_set_text_color_if_diff(ov_note, UI_TEXT_DIM);
    }

    lv_obj_add_flag(edit_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(note_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(list_view, LV_OBJ_FLAG_HIDDEN);
    list_refresh_all();
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ov);
}

/* ---------- 响铃遮罩：全屏闪烁 + 点屏停止 ---------- */

static lv_obj_t *ring_ov;
static lv_obj_t *ring_time;
static lv_obj_t *ring_tip;
static lv_timer_t *ring_timer;

static void ring_stop_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    app_alarm_stop();
}

static void ring_timer_cb(lv_timer_t *timer)
{
    RT_UNUSED(timer);

    if (g_app.alarm_ring)
    {
        int h = (int)((g_app.alarm_ring_ts / 3600) % 24);
        int m = (int)((g_app.alarm_ring_ts / 60) % 60);
        int flash = (int)((lv_tick_get() / 500) % 2);

        if (lv_obj_has_flag(ring_ov, LV_OBJ_FLAG_HIDDEN))
        {
            lv_obj_clear_flag(ring_ov, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(ring_ov);   /* 盖过一切（含二级界面） */
            screen_saver_notify_activity();    /* 息屏中被叫醒：立即点亮背光 */
        }

        /* 响铃期间保持常亮：重置 LVGL 非活动时钟，屏保就不会关背光；
         * 停止后不再续期，60s 恢复正常息屏节奏 */
        lv_display_trigger_activity(NULL);

        ui_label_set_text_fmt_if_diff(ring_time, "%02d:%02d", h, m);
        ui_set_bg_color_if_diff(ring_ov, flash ? UI_ATTENTION : UI_BG);
    }
    else if (!lv_obj_has_flag(ring_ov, LV_OBJ_FLAG_HIDDEN))
    {
        lv_obj_add_flag(ring_ov, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_alarm_ring_create(lv_obj_t *parent)
{
    ring_ov = lv_obj_create(parent);
    lv_obj_set_size(ring_ov, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_style_bg_color(ring_ov, lv_color_hex(UI_ATTENTION), 0);
    lv_obj_set_style_bg_opa(ring_ov, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ring_ov, 0, 0);
    lv_obj_set_style_radius(ring_ov, 0, 0);
    lv_obj_set_style_pad_all(ring_ov, 0, 0);
    lv_obj_clear_flag(ring_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ring_ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ring_ov, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(ring_ov, ring_stop_cb, LV_EVENT_CLICKED, NULL);

    ring_time = lv_label_create(ring_ov);
    lv_obj_set_style_text_font(ring_time, UI_FONT_CLOCK, 0);
    lv_obj_set_style_text_color(ring_time, lv_color_hex(UI_INK), 0);
    lv_label_set_text(ring_time, "--:--");
    lv_obj_center(ring_time);

    ring_tip = lv_label_create(ring_ov);
    lv_obj_set_style_text_font(ring_tip, UI_FONT_TEXT, 0);
    lv_obj_set_style_text_color(ring_tip, lv_color_hex(UI_TEXT_DIM), 0);
    lv_label_set_text(ring_tip, "Tap to stop");
    lv_obj_align(ring_tip, LV_ALIGN_CENTER, 0, 44);

    ring_timer = lv_timer_create(ring_timer_cb, 200, NULL);
    RT_UNUSED(ring_timer);
}
