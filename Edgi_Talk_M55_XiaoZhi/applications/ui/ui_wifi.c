/*
 * ui_wifi.c - 屏上 WiFi 配网界面
 *
 * 宿主：ui_pages_init() 里 ui_wifi_create(scr)，最后创建以保证盖住一切。
 *
 * 布局（800x480，参考 ui_app.c 的条带比例）：
 *   y   0   bar 56px：Back / "WiFi" / 状态文字 / Rescan / Disconnect
 *   y  56   sep 1px
 *   y  57   列表区 800x423：每行 800x64，最多显示 6 行，纵向滚动
 *
 * 输入态（点某行后）：
 *   全屏半透明遮罩 + 底部键盘面板。
 *   y   0   标题 "Connect to <ssid>"
 *   y  60   password textarea（带眼睛切换明文）
 *   y 140   Connect / Cancel 两个按钮
 *   y 200   lv_keyboard 占满剩余 280px
 *
 * 中文 SSID 的显示：
 *   lv_conf.h 的 LV_FONT_SIMSUN_16_CJK 若为 0，字体不含 CJK 字形，
 *   中文 SSID 会显示为空或方框。因此此处用 #if LV_FONT_SIMSUN_16_CJK
 *   选择字体：开启时使用 16px CJK，未开启时退回 montserrat 16（ASCII 正常，
 *   中文不显示但不影响选择，SSID 值本身仍是完整字节串）。
 *
 * 刷新：500ms 定时器，只读快照并用 if_diff 刷新文本，不重建控件
 * （例外是扫描状态从 RUNNING 变为 DONE 时重建列表，这是必要的）。
 */
#include <rtthread.h>
#include <string.h>

#include "lvgl.h"
#include "ui_theme.h"
#include "ui_wifi.h"
#include "web_cfg.h"
#include "app_cfg.h"

/* rt_wlan_get_rssi：状态行要显示实时信号强度 */
#ifdef RT_USING_WIFI
#include <wlan_mgnt.h>
#endif

/* SSID 用的字体。
 *
 * 2026-10-26 起 lv_conf.h 的 LV_FONT_SIMSUN_16_CJK 已打开，此处会走
 * simsun 分支。但需要明确其覆盖范围（实测字模表）：
 *   该字体仅含 1187 个稀疏选择的字符，其中汉字 1118 个，属于"最常见字"
 *   子集，并非全量 GB2312。实测缺失的常用字包括：网 络 连 接 设 页 扫
 *   败 开 关 无 线 测 试 华 为 红 厅 卧 办 楼 巴 等。
 *
 * 由此确定两条约束：
 *   1) 界面文案一律使用 ASCII（本文件所有 label 文本均为英文），
 *      因为中文文案几乎必然遇到缺字，会出现"网格连接"这类乱码。
 *   2) 字体仅用于渲染用户数据（SSID），缺字时该字符显示为空白，
 *      不影响其它字符，也不影响连接，SSID 的字节串始终完整。
 *
 * 2026-10-27 起改用 UI_FONT_TEXT：该宏现指向从 SD 卡运行时加载的
 * 全量 GB2312 字库（见 cjk_font.c），覆盖 6763 个汉字，SSID 中的中文
 * 基本不再缺字。字库加载失败时会自动退回编译期的 Montserrat。 */
#define WIFI_FONT      UI_FONT_TEXT

#define WIFI_BAR_H      56
#define WIFI_ROW_H      64
#define WIFI_ROW_MAX    6        /* 一屏 6 行，其余靠滚动 */
#define WIFI_REFRESH_MS 500

/* 输入态面板。
 *
 * 布局（800x480 总高，键盘底部对齐）：
 *   y  14  标题 "Connect to <ssid>"
 *   y  52  密码框（高 52）
 *   y 112  Connect / Cancel（高 52，底边 164）
 *   y 170  lv_keyboard 高 WIFI_KB_H=306，底边正好 476，余 4px
 *
 * 306 = 480 - 174，有意让键盘占用全部剩余高度：4 行按键在
 * LV_DPI_DEF=130 下每行约 58px，4 行加行间隙与内边距约为 260，
 * 306 留有足够余量，不会出现最后一行被容器裁掉的情况。 */
#define WIFI_KB_H       306
#define WIFI_TA_Y       52
#define WIFI_BTN_Y      112

static lv_obj_t *w_root;         /* 全屏根 */
static lv_obj_t *w_state;        /* 顶部状态文字 */
static lv_obj_t *w_list;         /* 列表容器（可滚动） */
static lv_obj_t *w_hint;         /* 列表为空/扫描中时的居中提示 */
static lv_timer_t *w_timer;

/* 输入态 */
static lv_obj_t *w_input;        /* 输入态根（含遮罩） */
static lv_obj_t *w_in_title;     /* 输入态标题（也兼作错误提示） */
static lv_obj_t *w_ta;           /* 密码输入框 */
static lv_obj_t *w_kb;           /* 键盘 */

static rt_bool_t w_open = RT_FALSE;

/* 列表条目：静态数组避免反复 malloc（LVGL 堆紧张） */
static lv_obj_t *w_rows[WEB_CFG_SCAN_MAX];
static char      w_row_ssid[WEB_CFG_SCAN_MAX][WEB_CFG_SSID_MAX];
static rt_bool_t w_row_secure[WEB_CFG_SCAN_MAX];
static int       w_row_cnt;
static int       w_drawn_scan_state = -1;   /* 上次绘制的扫描状态 */

/* 当前选中的 SSID（输入态用） */
static char w_sel_ssid[WEB_CFG_SSID_MAX];
static rt_bool_t w_sel_secure;

/* 扫描快照的本地副本。置于文件作用域而非定时器内的 static：
 * 20 条 × 44 字节 = 880 字节，LVGL 线程栈仅 24KB 且正在执行渲染与动画，
 * 不能放入栈。此处是唯一写者（定时器），不存在并发问题。 */
static web_cfg_ap_t w_aps[WEB_CFG_SCAN_MAX];

/* ===================== 小工具 ===================== */

static lv_obj_t *w_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, const char *txt,
                       int dark, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_t *lab = lv_label_create(btn);

    lv_obj_set_size(btn, w, h);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

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

    lv_label_set_text(lab, txt);
    lv_obj_set_style_text_font(lab, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(dark ? UI_CARD_BG : UI_INK), 0);
    lv_obj_center(lab);

    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    return btn;
}

/* ===================== 输入态 ===================== */

static void wifi_input_close(void)
{
    if (w_input != RT_NULL)
        lv_obj_add_flag(w_input, LV_OBJ_FLAG_HIDDEN);
}

static void wifi_input_cancel_cb(lv_event_t *e)
{
    RT_UNUSED(e);
    wifi_input_close();
}

static void wifi_eye_cb(lv_event_t *e)
{
    RT_UNUSED(e);
    if (w_ta == RT_NULL) return;

    /* 在明文/星号间切换。lv_textarea 用 LV_TEXTAREA_PASSWORD_MODE 控制 */
    if (lv_textarea_get_password_mode(w_ta))
        lv_textarea_set_password_mode(w_ta, false);
    else
        lv_textarea_set_password_mode(w_ta, true);
}

static void wifi_connect_cb(lv_event_t *e)
{
    const char *pass;

    RT_UNUSED(e);

    if (w_ta == RT_NULL) return;

    pass = lv_textarea_get_text(w_ta);

    /* 密码空且是加密网络：允许提交（让用户先试，可能密码就是空），
     * 但给个提示。真正校验在固件侧（whd 会拒）。
     *
     * 失败反馈：输入态里已经没有独立提示行了（高度全给键盘），
     * 所以改用标题变红 + 文案，用户一眼能看到，也不占空间。 */
    if (web_cfg_connect_begin(w_sel_ssid, pass) != 0)
    {
        if (w_in_title != RT_NULL)
        {
            lv_label_set_text(w_in_title, "Invalid - try again");
            lv_obj_set_style_text_color(w_in_title, lv_color_hex(UI_ATTENTION), 0);
        }
        return;
    }

    wifi_input_close();

    if (w_state != RT_NULL)
        ui_label_set_text_if_diff(w_state, "connecting...");
}

static void wifi_row_cb(lv_event_t *e)
{
    int idx = (int)(rt_base_t)lv_event_get_user_data(e);

    if (idx < 0 || idx >= w_row_cnt) return;

    rt_strncpy(w_sel_ssid, w_row_ssid[idx], sizeof(w_sel_ssid) - 1);
    w_sel_ssid[sizeof(w_sel_ssid) - 1] = '\0';
    w_sel_secure = w_row_secure[idx];

    if (w_input == RT_NULL) return;

    /* 标题 "Connect to <ssid>"。每次进入都复位颜色，因为上次可能因
     * 提交失败被改为橙色。 */
    if (w_in_title != RT_NULL)
    {
        char buf[64];
        rt_snprintf(buf, sizeof(buf), "Connect to %s", w_sel_ssid);
        lv_label_set_text(w_in_title, buf);
        lv_obj_set_style_text_color(w_in_title, lv_color_hex(UI_INK), 0);
    }

    /* 每次重新打开都清空密码框，避免上一次的残留 */
    if (w_ta != RT_NULL)
    {
        lv_textarea_set_text(w_ta, "");
    }

    lv_obj_clear_flag(w_input, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(w_input);

    /* 弹出键盘：不自动弹（会遮住标题），让用户点输入框再弹 */
    if (w_kb != RT_NULL)
        lv_obj_add_flag(w_kb, LV_OBJ_FLAG_HIDDEN);
}

/* 点输入框后显示键盘 */
static void wifi_ta_click_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    if (w_kb != RT_NULL)
    {
        lv_obj_clear_flag(w_kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(w_kb, w_ta);
    }
}

/* 键盘左下角的关闭键（LV_EVENT_CANCEL）仅收起键盘，不取消整个对话框。
 * 不能复用 wifi_ta_click_cb：该回调用于显示键盘，若挂在 CANCEL 上，
 * 用户想收起键盘时反而会再次显示。 */
static void wifi_kb_cancel_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    if (w_kb != RT_NULL)
        lv_obj_add_flag(w_kb, LV_OBJ_FLAG_HIDDEN);
}

/* ===================== 列表 ===================== */

static void wifi_list_clear(void)
{
    int i;

    for (i = 0; i < w_row_cnt; i++)
    {
        if (w_rows[i] != RT_NULL)
        {
            lv_obj_delete(w_rows[i]);
            w_rows[i] = RT_NULL;
        }
    }
    w_row_cnt = 0;
}

/* 信号强度转换为文字与颜色。按 dBm 分档比绘制信号格更简单，
 * 且小屏上数字比格子更易读。 */
static const char *rssi_bar(int rssi)
{
    if (rssi >= -55) return "||||";     /* 强 */
    if (rssi >= -67) return "|||";
    if (rssi >= -78) return "||";
    return "|";
}

static void wifi_build_rows(web_cfg_ap_t *aps, int n)
{
    int i;

    wifi_list_clear();

    if (w_hint != RT_NULL)
    {
        if (n == 0)
        {
            ui_label_set_text_if_diff(w_hint, "No network found. Tap Rescan.");
            lv_obj_clear_flag(w_hint, LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            lv_obj_add_flag(w_hint, LV_OBJ_FLAG_HIDDEN);
        }
    }

    for (i = 0; i < n && i < WEB_CFG_SCAN_MAX; i++)
    {
        lv_obj_t *row = lv_obj_create(w_list);
        lv_obj_t *name, *sig, *lock;
        lv_coord_t y = (lv_coord_t)(i * WIFI_ROW_H);

        lv_obj_set_size(row, 800, WIFI_ROW_H - 1);
        lv_obj_set_pos(row, 0, y);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_BG), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, wifi_row_cb, LV_EVENT_CLICKED,
                            (void *)(rt_base_t)i);

        /* SSID：CJK 字体没开时中文不显示，但值本身完整 */
        name = lv_label_create(row);
        lv_label_set_text(name, aps[i].ssid);
        lv_obj_set_style_text_font(name, WIFI_FONT, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(UI_INK), 0);
        lv_obj_set_width(name, 520);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(name, UI_PAD, 20);

        /* 加密标记：只在加密网络显示。
         * 用 ASCII 的 "*" 而不是 LV_SYMBOL_* 图标：符号字形依赖
         * FontAwesome 私有区，在切换字体时容易缺失，而列表里已经
         * 有信号强度数字，多一个星号足够表意。 */
        if (aps[i].secure)
        {
            lock = lv_label_create(row);
            lv_label_set_text(lock, "*");
            lv_obj_set_style_text_font(lock, UI_FONT_SMALL, 0);
            lv_obj_set_style_text_color(lock, lv_color_hex(UI_TEXT_DIM), 0);
            lv_obj_set_pos(lock, 800 - UI_PAD - 152, 20);
        }

        /* 信号强度：右侧 */
        sig = lv_label_create(row);
        lv_label_set_text_fmt(sig, "%s  %d", rssi_bar(aps[i].rssi), aps[i].rssi);
        lv_obj_set_style_text_font(sig, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(sig,
            lv_color_hex(aps[i].rssi >= -67 ? UI_INK : UI_TEXT_DIM), 0);
        lv_obj_set_width(sig, 120);
        lv_obj_set_style_text_align(sig, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(sig, 800 - UI_PAD - 120, 22);

        /* 行底发丝线 */
        {
            lv_obj_t *sep = lv_obj_create(row);
            lv_obj_set_size(sep, 800 - UI_PAD * 2, 1);
            lv_obj_set_pos(sep, UI_PAD, WIFI_ROW_H - 2);
            lv_obj_set_style_bg_color(sep, lv_color_hex(UI_SEP), 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(sep, 0, 0);
            lv_obj_set_style_radius(sep, 0, 0);
            lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        }

        /* 用 memcpy 而不是 strncpy：aps[i].ssid 已在 web_cfg 侧保证
         * 以 '\0' 结尾，最长 WEB_CFG_SSID_MAX-1。strncpy 会让 GCC 报
         * -Wstringop-truncation（它无法证明源一定短于目标），而显式
         * 拷 + 补 '\0' 既无警告也表达了真实意图。 */
        {
            rt_size_t slen = rt_strlen(aps[i].ssid);

            if (slen > WEB_CFG_SSID_MAX - 1) slen = WEB_CFG_SSID_MAX - 1;
            rt_memcpy(w_row_ssid[i], aps[i].ssid, slen);
            w_row_ssid[i][slen] = '\0';
        }
        w_row_secure[i] = aps[i].secure;

        w_rows[i] = row;
        w_row_cnt = i + 1;
    }
}

/* ===================== 定时刷新 ===================== */

static void wifi_timer_cb(lv_timer_t *t)
{
    int state, n = 0;
    int wstate;

    RT_UNUSED(t);

    if (!w_open) return;

    /* 界面开着就保持亮屏 */
    lv_display_trigger_activity(NULL);

    wstate = web_cfg_get_state();

    /* 顶部状态文字 */
    switch (wstate)
    {
    case 0: ui_label_set_text_if_diff(w_state, "not connected"); break;
    case 1: ui_label_set_text_if_diff(w_state, "connecting..."); break;
    case 2:
    {
        char buf[48];
        rt_snprintf(buf, sizeof(buf), "connected  %d dBm", rt_wlan_get_rssi());
        ui_label_set_text_if_diff(w_state, buf);
        break;
    }
    case 3: ui_label_set_text_if_diff(w_state, "setup AP running"); break;
    default: ui_label_set_text_if_diff(w_state, "-"); break;
    }

    /* SD 卡不在或无法写入时，配置仅在内存有效，重启后丢失。该状态必须
     * 让用户看到，否则"连上了、重启又要重配"会难以解释。
     * 优先级高于连接状态：连接成功但未保存属于需要提醒的情况。 */
    if (!app_cfg_persist_ok() && wstate != 1)
    {
        ui_label_set_text_if_diff(w_state, "no SD card - not saved");
    }

    /* 注意 ui_set_text_color_if_diff 收的是 24 位 0xRRGGBB，不是 lv_color_t
     * （后者是 RGB565 或 ARGB8888 结构体，直接传会编译失败） */
    ui_set_text_color_if_diff(w_state,
        (!app_cfg_persist_ok() && wstate != 1) ? UI_ATTENTION :
        ((wstate == 2) ? UI_INK : ((wstate == 1) ? UI_ATTENTION : UI_TEXT_DIM)));

    /* 扫描结果：只在状态变化时重建列表，避免每 500ms 重建控件 */
    state = web_cfg_scan_snapshot(w_aps, WEB_CFG_SCAN_MAX, &n);

    if (state == WEB_CFG_SCAN_RUNNING)
    {
        if (w_drawn_scan_state != WEB_CFG_SCAN_RUNNING)
        {
            wifi_list_clear();
            if (w_hint != RT_NULL)
            {
                ui_label_set_text_if_diff(w_hint, "Scanning...");
                lv_obj_clear_flag(w_hint, LV_OBJ_FLAG_HIDDEN);
            }
            w_drawn_scan_state = WEB_CFG_SCAN_RUNNING;
        }
        return;
    }

    if (state == WEB_CFG_SCAN_DONE)
    {
        /* DONE 且尚未绘制时建立列表。扫描期间 UI 可能已重新扫描过一次，
         * 用 w_drawn_scan_state 区分，避免重复创建。 */
        if (w_drawn_scan_state != WEB_CFG_SCAN_DONE)
        {
            wifi_build_rows(w_aps, n);
            w_drawn_scan_state = WEB_CFG_SCAN_DONE;
        }
        return;
    }

    if (state == WEB_CFG_SCAN_FAIL)
    {
        if (w_drawn_scan_state != WEB_CFG_SCAN_FAIL)
        {
            wifi_list_clear();
            if (w_hint != RT_NULL)
            {
                ui_label_set_text_if_diff(w_hint, "Scan failed. Tap Rescan.");
                lv_obj_clear_flag(w_hint, LV_OBJ_FLAG_HIDDEN);
            }
            w_drawn_scan_state = WEB_CFG_SCAN_FAIL;
        }
        return;
    }

    /* IDLE：还没扫过 */
    if (w_drawn_scan_state != WEB_CFG_SCAN_IDLE)
    {
        wifi_list_clear();
        if (w_hint != RT_NULL)
        {
            ui_label_set_text_if_diff(w_hint, "Tap Rescan to search.");
            lv_obj_clear_flag(w_hint, LV_OBJ_FLAG_HIDDEN);
        }
        w_drawn_scan_state = WEB_CFG_SCAN_IDLE;
    }
}

/* ===================== 动作 ===================== */

static void wifi_back_cb(lv_event_t *e)
{
    RT_UNUSED(e);
    ui_wifi_close();
}

static void wifi_rescan_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    /* 强制重画：把状态推回 IDLE 之外的"未知"，让定时器去建新列表 */
    w_drawn_scan_state = -1;

    if (web_cfg_scan_begin() != 0)
    {
        /* 正在扫描或已连接；已连接时提示用户 */
        if (web_cfg_get_state() == 2 && w_state != RT_NULL)
            ui_label_set_text_if_diff(w_state, "disconnect first to scan");
    }
}

static void wifi_disc_cb(lv_event_t *e)
{
    RT_UNUSED(e);

    web_cfg_disconnect();
    w_drawn_scan_state = -1;
}

/* ===================== 构建 ===================== */

static void wifi_build_input(lv_obj_t *root)
{
    w_input = lv_obj_create(root);
    lv_obj_set_size(w_input, 800, 480);
    lv_obj_set_pos(w_input, 0, 0);
    lv_obj_set_style_bg_color(w_input, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(w_input, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(w_input, 0, 0);
    lv_obj_set_style_radius(w_input, 0, 0);
    lv_obj_set_style_pad_all(w_input, 0, 0);
    lv_obj_clear_flag(w_input, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w_input, LV_OBJ_FLAG_HIDDEN);

    w_in_title = ui_label(w_input, &ui_style_text);
    lv_label_set_text(w_in_title, "Connect to ...");
    lv_obj_set_pos(w_in_title, UI_PAD, 14);
    lv_obj_set_width(w_in_title, 800 - UI_PAD * 2 - 100);
    /* 单行加省略号：SSID 可能较长，不允许换行打乱下方控件布局 */
    lv_label_set_long_mode(w_in_title, LV_LABEL_LONG_DOT);

    /* 密码框。高度 52 与按钮一致，视觉上成组 */
    w_ta = lv_textarea_create(w_input);
    lv_textarea_set_one_line(w_ta, true);
    lv_textarea_set_password_mode(w_ta, true);
    lv_textarea_set_max_length(w_ta, APP_CFG_PASS_LEN - 1);
    lv_textarea_set_placeholder_text(w_ta, "password");
    lv_obj_set_size(w_ta, 560, 52);
    lv_obj_set_pos(w_ta, UI_PAD, WIFI_TA_Y);
    lv_obj_set_style_radius(w_ta, 0, 0);
    lv_obj_set_style_text_font(w_ta, WIFI_FONT, 0);
    /* 输入框内边距同样按 DPI 放大过，适当缩小以使文字垂直居中 */
    lv_obj_set_style_pad_top(w_ta, 10, 0);
    lv_obj_set_style_pad_bottom(w_ta, 10, 0);
    lv_obj_add_event_cb(w_ta, wifi_ta_click_cb, LV_EVENT_CLICKED, RT_NULL);

    /* 错误提示：按钮下方与键盘之间的空隙仅 6px（164..170），无法容纳
     * 独立行，因此不创建控件；错误提示改为调整标题颜色。 */

    /* 眼睛按钮：切换明文显示（与密码框同高，紧贴其右） */
    {
        lv_obj_t *eye = w_btn(w_input, UI_PAD + 568, WIFI_TA_Y, 52, 52,
                              "AB", 0, wifi_eye_cb, RT_NULL);
        RT_UNUSED(eye);
    }

    /* Connect / Cancel */
    w_btn(w_input, UI_PAD, WIFI_BTN_Y, 260, 52, "Connect", 1,
          wifi_connect_cb, RT_NULL);
    w_btn(w_input, UI_PAD + 276, WIFI_BTN_Y, 260, 52, "Cancel", 0,
          wifi_input_cancel_cb, RT_NULL);

    /* 键盘：4 行按键（LVGL 小写模式 11/10/12/4 键）。
     *
     * 2026-10-26 修正"键盘显示不完全"：原先 280px 的固定高度不足。
     * 原因：LV_DPI_DEF=130 使 buttonmatrix 的默认内边距偏大，
     * 4 行 ×（字高 16 + 上下 pad）再加行间隙已接近 280，最后一行
     * 会超出容器底部而被裁掉。
     *
     * 三处修正：
     *   1) 高度提高到 WIFI_KB_H=306，占用 174..480 的全部剩余空间；
     *   2) 改为底部对齐（LV_ALIGN_BOTTOM_MID）而非手算 480-高度，
     *      将来屏幕高度变化也不会越界；
     *   3) 将键盘自身的 pad 从 DPI 推算值缩小到 4px，这是"显示不全"的
     *      直接原因，4 行 × 约 12px 的上下 pad 即浪费 96px。
     *
     * 另需清除 LV_OBJ_FLAG_SCROLLABLE：buttonmatrix 内容超出时默认允许
     * 滚动，用户会看到键盘被推着上下滑动，误以为显示不全。 */
    w_kb = lv_keyboard_create(w_input);
    lv_obj_set_size(w_kb, 800, WIFI_KB_H);
    lv_obj_align(w_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(w_kb, 4, 0);
    lv_obj_set_style_pad_row(w_kb, 4, 0);
    lv_obj_set_style_pad_column(w_kb, 4, 0);
    lv_obj_clear_flag(w_kb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(w_kb, LV_SCROLLBAR_MODE_OFF);
    lv_keyboard_set_mode(w_kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(w_kb, w_ta);
    lv_obj_add_flag(w_kb, LV_OBJ_FLAG_HIDDEN);

    /* 键盘的 OK 键 = 直接连接 */
    lv_obj_add_event_cb(w_kb, wifi_connect_cb, LV_EVENT_READY, RT_NULL);
    /* 键盘的关闭键 = 只收起键盘（不取消整个对话框） */
    lv_obj_add_event_cb(w_kb, wifi_kb_cancel_cb, LV_EVENT_CANCEL, RT_NULL);
}

static void wifi_build(lv_obj_t *scr)
{
    w_root = lv_obj_create(scr);
    lv_obj_set_size(w_root, 800, 480);
    lv_obj_set_pos(w_root, 0, 0);
    lv_obj_set_style_bg_color(w_root, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(w_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(w_root, 0, 0);
    lv_obj_set_style_radius(w_root, 0, 0);
    lv_obj_set_style_pad_all(w_root, 0, 0);
    lv_obj_clear_flag(w_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(w_root, LV_OBJ_FLAG_HIDDEN);

    /* 顶部条 */
    w_btn(w_root, UI_PAD, 10, 80, 36, "Back", 1, wifi_back_cb, RT_NULL);

    {
        lv_obj_t *title = ui_label(w_root, &ui_style_text);
        lv_label_set_text(title, "WiFi");
        lv_obj_set_pos(title, UI_PAD + 96, 18);
    }

    /* 状态文字：标题右侧 */
    w_state = ui_label(w_root, &ui_style_small);
    lv_label_set_text(w_state, "-");
    lv_obj_set_pos(w_state, UI_PAD + 170, 21);
    lv_obj_set_width(w_state, 280);
    lv_label_set_long_mode(w_state, LV_LABEL_LONG_DOT);

    /* 右侧两个按钮 */
    w_btn(w_root, 800 - UI_PAD - 110, 10, 110, 36, "Disconnect", 0,
          wifi_disc_cb, RT_NULL);
    w_btn(w_root, 800 - UI_PAD - 110 - 96 - 8, 10, 96, 36, "Rescan", 1,
          wifi_rescan_cb, RT_NULL);

    /* 分隔线 */
    {
        lv_obj_t *sep = lv_obj_create(w_root);
        lv_obj_set_size(sep, 800 - UI_PAD * 2, 1);
        lv_obj_set_pos(sep, UI_PAD, WIFI_BAR_H);
        lv_obj_set_style_bg_color(sep, lv_color_hex(UI_SEP), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sep, 0, 0);
        lv_obj_set_style_radius(sep, 0, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    /* 列表容器：纵向滚动，条目由 wifi_build_rows 添加 */
    w_list = lv_obj_create(w_root);
    lv_obj_set_size(w_list, 800, 480 - WIFI_BAR_H - 2);
    lv_obj_set_pos(w_list, 0, WIFI_BAR_H + 2);
    lv_obj_set_style_bg_opa(w_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(w_list, 0, 0);
    lv_obj_set_style_radius(w_list, 0, 0);
    lv_obj_set_style_pad_all(w_list, 0, 0);
    lv_obj_set_scroll_dir(w_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(w_list, LV_SCROLLBAR_MODE_AUTO);

    /* 居中提示（扫描中 / 无结果 / 失败共用） */
    w_hint = ui_label(w_root, &ui_style_small_dim);
    lv_label_set_text(w_hint, "Tap Rescan to search.");
    lv_obj_set_width(w_hint, 600);
    lv_obj_set_style_text_align(w_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(w_hint, 100, 240);

    wifi_build_input(w_root);

    w_timer = lv_timer_create(wifi_timer_cb, WIFI_REFRESH_MS, RT_NULL);
    lv_timer_pause(w_timer);
}

/* ===================== 对外 ===================== */

void ui_wifi_create(lv_obj_t *scr)
{
    if (w_root != RT_NULL || scr == RT_NULL) return;
    wifi_build(scr);
}

void ui_wifi_open(void)
{
    if (w_root == RT_NULL || w_open) return;

    w_open = RT_TRUE;
    w_drawn_scan_state = -1;

    lv_obj_clear_flag(w_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(w_root);

    if (w_timer != RT_NULL) lv_timer_resume(w_timer);

    lv_display_trigger_activity(NULL);

    /* 打开即扫一次。若已连接则扫不了（web_cfg_scan_begin 返回非 0），
     * 此时定时器会把状态显示成 connected 并保留空列表。 */
    web_cfg_scan_begin();

    /* 立即刷一帧，避免打开瞬间是空白（不等 500ms） */
    wifi_timer_cb(RT_NULL);
}

void ui_wifi_close(void)
{
    if (w_root == RT_NULL || !w_open) return;

    wifi_input_close();
    w_open = RT_FALSE;

    if (w_timer != RT_NULL) lv_timer_pause(w_timer);
    lv_obj_add_flag(w_root, LV_OBJ_FLAG_HIDDEN);
}

rt_bool_t ui_wifi_is_open(void)
{
    return w_open;
}
