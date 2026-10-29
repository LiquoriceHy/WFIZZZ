/*
 * ui_lanctl.c - LAN Devices 全屏应用页
 *
 * 宿主契约（ui_app.c）：draw 在打开时调用一次，之后每 500ms 调用一次，
 * 必须保证重复调用安全：static 指针判 NULL 后建立骨架，之后仅通过
 * if_diff 刷新文本与颜色；undraw 只清指针，控件由宿主 lv_obj_clean 统一删除。
 *
 * 布局（app_body 内容区 752x374，不可滚动）：
 *   y   0   MQTT 状态行（左） + 多设备切换 chips（右，仅 >1 台时画）
 *   y  26   设备卡 752x116：名称 / id@ip / 在线方块+状态 / seen / 最近回包
 *   y 152   "Commands" 标题
 *   y 174   命令按钮 3 列 x 2 行（240x56，间距 16）
 *   y 318   最近下发行（tx）
 *
 * 命令按钮按下即调 lanctl_dev_send（LVGL 线程直接 publish，paho pipe
 * 模式线程安全）；按钮组随选中设备重建（设备数与命令都是编译期常量，
 * 重建只发生在切换设备时）。
 *
 * 文案必须 ASCII：CJK 字体未链接，中文整段不绘制。
 */
#include <rtthread.h>
#include <string.h>

#include "lvgl.h"
#include "ui_theme.h"
#include "lanctl.h"

#define LUI_CMD_W    240
#define LUI_CMD_H    56
#define LUI_CMD_GAP  16
#define LUI_CMD_ROW  3          /* 每行按钮数 */
#define LUI_CMD_Y    174

/* 布局 y 坐标：MQTT 行 0，卡 26..142，Commands 标题 152，按钮 174..302，
 * tx 行 318；全部落在 374 高度内 */
#define LUI_CARD_Y    26
#define LUI_CARD_H    116
#define LUI_CAP_Y     152
#define LUI_TX_Y      318

static lv_obj_t *lui_mqtt;                 /* MQTT 连接状态行 */
static lv_obj_t *lui_card;                 /* 设备卡 */
static lv_obj_t *lui_name, *lui_meta;      /* 设备名 / id@ip */
static lv_obj_t *lui_state;                /* ONLINE / OFFLINE / NO SIG */
static lv_obj_t *lui_dot;                  /* 在线状态小方块 */
static lv_obj_t *lui_seen;                 /* seen Ns ago */
static lv_obj_t *lui_reply;                /* 最近回包 */
static lv_obj_t *lui_tx;                   /* 最近下发 */
static lv_obj_t *lui_body;                 /* app_body，重建按钮时挂用 */

static lv_obj_t *lui_btns[LANCTL_CMD_MAX];
static int       lui_btn_id[LANCTL_CMD_MAX];
static int       lui_btn_cnt;
static int       lui_btn_dev = -1;         /* 当前按钮组属于的设备下标 */

static lv_obj_t *lui_chips[LANCTL_DEV_MAX];
static int       lui_chip_id[LANCTL_DEV_MAX];
static int       lui_chip_cnt;
static int       lui_chip_sel = -1;

static int       lui_sel;                  /* 选中设备（undraw 后保留） */

/* 墨黑（白字）/ 白底描边按钮工厂（抄 ui_alarm.c 的 btn_make） */
static lv_obj_t *lui_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
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

static void lui_cmd_cb(lv_event_t *e)
{
    int cmd = *(int *)lv_event_get_user_data(e);

    lanctl_dev_send(lui_sel, cmd);
    /* last_cmd 已记录在快照里，下一次 500ms 刷新即上屏 */
}

static void lui_chip_cb(lv_event_t *e)
{
    lui_sel = *(int *)lv_event_get_user_data(e);
    /* 按钮组/chips 的重建在 refresh 里按 dev 变化触发 */
}

static void lui_build_btns(void)
{
    int i, cnt;

    for (i = 0; i < lui_btn_cnt; i++)
        if (lui_btns[i] != RT_NULL)
            lv_obj_delete(lui_btns[i]);
    lui_btn_cnt = 0;
    for (i = 0; i < LANCTL_CMD_MAX; i++)
        lui_btns[i] = RT_NULL;

    cnt = lanctl_dev_cmd_count(lui_sel);
    for (i = 0; i < cnt && i < LANCTL_CMD_MAX; i++)
    {
        int col = i % LUI_CMD_ROW, row = i / LUI_CMD_ROW;

        lui_btn_id[i] = i;
        lui_btns[i] = lui_btn(lui_body,
                              col * (LUI_CMD_W + LUI_CMD_GAP),
                              LUI_CMD_Y + row * (LUI_CMD_H + LUI_CMD_GAP),
                              LUI_CMD_W, LUI_CMD_H,
                              lanctl_dev_cmd_label(lui_sel, i),
                              1, lui_cmd_cb, &lui_btn_id[i]);
    }
    lui_btn_cnt = i;
    lui_btn_dev = lui_sel;
}

static void lui_build_chips(void)
{
    int i, cnt, dark;

    for (i = 0; i < lui_chip_cnt; i++)
        if (lui_chips[i] != RT_NULL)
            lv_obj_delete(lui_chips[i]);
    lui_chip_cnt = 0;
    for (i = 0; i < LANCTL_DEV_MAX; i++)
        lui_chips[i] = RT_NULL;

    cnt = lanctl_dev_count();
    if (cnt <= 1)
    {
        lui_chip_sel = cnt == 1 ? 0 : -1;
        return;                       /* 单设备不需要切换行 */
    }

    /* 靠右排布：chips 占宽 = cnt * 126 - 16 */
    for (i = 0; i < cnt && i < LANCTL_DEV_MAX; i++)
    {
        lanctl_dev_snapshot_t snap;

        lanctl_dev_snapshot(i, &snap);
        dark = (i == lui_sel);
        lui_chip_id[i] = i;
        lui_chips[i] = lui_btn(lui_body,
                               752 - (cnt - i) * (110 + LUI_CMD_GAP), -2,
                               110, 32, snap.name,
                               dark, lui_chip_cb, &lui_chip_id[i]);
    }
    lui_chip_cnt = i;
    lui_chip_sel = lui_sel;
}

static void lui_build(lv_obj_t *parent)
{
    lui_body = parent;

    lui_mqtt = ui_label(parent, &ui_style_small_dim);
    lv_obj_set_pos(lui_mqtt, 0, 0);
    lv_obj_set_width(lui_mqtt, 380);
    lv_label_set_long_mode(lui_mqtt, LV_LABEL_LONG_DOT);
    lv_label_set_text(lui_mqtt, "-");

    if (lui_sel >= lanctl_dev_count())
        lui_sel = 0;
    lui_build_chips();

    lui_card = ui_card_create(parent, 752, LUI_CARD_H);
    lv_obj_set_pos(lui_card, 0, LUI_CARD_Y);

    lui_name = ui_label(lui_card, &ui_style_text);
    lv_obj_set_pos(lui_name, 16, 12);

    lui_meta = ui_label(lui_card, &ui_style_small_dim);
    lv_obj_set_pos(lui_meta, 16, 42);

    lui_dot = lv_obj_create(lui_card);
    lv_obj_set_size(lui_dot, 14, 14);
    lv_obj_set_pos(lui_dot, 752 - 16 - 150 - 26, 16);
    lv_obj_set_style_radius(lui_dot, 0, 0);
    lv_obj_set_style_border_width(lui_dot, 0, 0);
    lv_obj_set_style_bg_color(lui_dot, lv_color_hex(UI_MUTED), 0);
    lv_obj_clear_flag(lui_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lui_state = ui_label(lui_card, &ui_style_small);
    lv_obj_set_pos(lui_state, 752 - 16 - 150, 12);
    lv_obj_set_width(lui_state, 150);
    lv_obj_set_style_text_align(lui_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lui_state, "-");

    lui_seen = ui_label(lui_card, &ui_style_small_dim);
    lv_obj_set_pos(lui_seen, 752 - 16 - 150, 40);
    lv_obj_set_width(lui_seen, 150);
    lv_obj_set_style_text_align(lui_seen, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(lui_seen, "-");

    lui_reply = ui_label(lui_card, &ui_style_small);
    lv_obj_set_pos(lui_reply, 16, 72);
    lv_obj_set_width(lui_reply, 752 - 32);
    lv_label_set_long_mode(lui_reply, LV_LABEL_LONG_DOT);
    lv_label_set_text(lui_reply, "reply: -");

    lv_obj_t *cap = ui_label(parent, &ui_style_small_dim);
    lv_obj_set_pos(cap, 0, LUI_CAP_Y);
    lv_label_set_text(cap, "Commands");

    lui_tx = ui_label(parent, &ui_style_small_dim);
    lv_obj_set_pos(lui_tx, 0, LUI_TX_Y);
    lv_obj_set_width(lui_tx, 752);
    lv_label_set_long_mode(lui_tx, LV_LABEL_LONG_DOT);
    lv_label_set_text(lui_tx, "tx: -");

    lui_build_btns();
}

static void lui_refresh(void)
{
    lanctl_dev_snapshot_t snap;
    const char *state;
    rt_uint32_t dot_rgb, state_rgb;
    char buf[128];
    int i, up = 0, n = lanctl_dev_count();

    /* MQTT 状态行（顺带数在线设备） */
    if (!lanctl_is_running())
        rt_snprintf(buf, sizeof(buf), "MQTT stopped");
    else if (!lanctl_mqtt_online())
        rt_snprintf(buf, sizeof(buf), "MQTT connecting...");
    else
    {
        for (i = 0; i < n; i++)
        {
            lanctl_dev_snapshot(i, &snap);
            if (snap.online)
                up++;
        }
        rt_snprintf(buf, sizeof(buf), "MQTT online   %d/%d up", up, n);
    }
    ui_label_set_text_if_diff(lui_mqtt, buf);

    if (lui_sel >= n)
        lui_sel = 0;

    /* 设备切换 chips / 命令按钮：选中变了才重建（编译期常量，无闪烁源） */
    if (lui_chip_sel != lui_sel)
        lui_build_chips();
    if (lui_btn_dev != lui_sel)
        lui_build_btns();

    lanctl_dev_snapshot(lui_sel, &snap);

    ui_label_set_text_if_diff(lui_name, snap.name);
    rt_snprintf(buf, sizeof(buf), "%s @ %s", snap.id,
                snap.ip[0] ? snap.ip : "-");
    ui_label_set_text_if_diff(lui_meta, buf);

    /* 颜色语义：在线=柔青绿（卡内唯一彩色锚点）；离线=橙（需关注）；
     * 从未见过=灰。全走 if_diff，不触发无谓重绘 */
    if (snap.online)
    {
        state = "ONLINE";
        dot_rgb = UI_MEMO_KNOWN;
        state_rgb = UI_INK;
    }
    else if (snap.seen)
    {
        state = "OFFLINE";
        dot_rgb = UI_ATTENTION;
        state_rgb = UI_ATTENTION;
    }
    else
    {
        state = "NO SIG";
        dot_rgb = UI_MUTED;
        state_rgb = UI_TEXT_DIM;
    }
    ui_label_set_text_if_diff(lui_state, state);
    ui_set_text_color_if_diff(lui_state, state_rgb);
    ui_set_bg_color_if_diff(lui_dot, dot_rgb);

    if (snap.seen)
        rt_snprintf(buf, sizeof(buf), "seen %us ago", (unsigned)snap.age_s);
    else
        rt_snprintf(buf, sizeof(buf), "never seen");
    ui_label_set_text_if_diff(lui_seen, buf);

    rt_snprintf(buf, sizeof(buf), "reply: %s",
                snap.last_reply[0] ? snap.last_reply : "-");
    ui_label_set_text_if_diff(lui_reply, buf);

    rt_snprintf(buf, sizeof(buf), "tx: %s",
                snap.last_cmd[0] ? snap.last_cmd : "-");
    ui_label_set_text_if_diff(lui_tx, buf);
}

void ui_lanctl_draw(lv_obj_t *parent)
{
    if (lui_mqtt == RT_NULL)
        lui_build(parent);
    lui_refresh();
}

void ui_lanctl_undraw(lv_obj_t *parent)
{
    int i;

    RT_UNUSED(parent);

    /* 只清指针：控件由 ui_app 统一 lv_obj_clean。lui_sel 保留，
     * 下次打开回到上次选中的设备。 */
    lui_mqtt = RT_NULL;
    lui_card = RT_NULL;
    lui_name = lui_meta = RT_NULL;
    lui_state = RT_NULL;
    lui_dot = RT_NULL;
    lui_seen = RT_NULL;
    lui_reply = RT_NULL;
    lui_tx = RT_NULL;
    lui_body = RT_NULL;

    for (i = 0; i < LANCTL_CMD_MAX; i++)
        lui_btns[i] = RT_NULL;
    lui_btn_cnt = 0;
    lui_btn_dev = -1;

    for (i = 0; i < LANCTL_DEV_MAX; i++)
        lui_chips[i] = RT_NULL;
    lui_chip_cnt = 0;
    lui_chip_sel = -1;
}
