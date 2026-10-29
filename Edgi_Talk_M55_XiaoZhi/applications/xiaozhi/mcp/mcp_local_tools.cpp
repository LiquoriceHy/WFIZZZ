/*
 * mcp_local_tools.cpp - 使 AI 能够读写板上真实数据的 MCP 工具
 *
 * 调用链路（已接通，本文件仅补充最后一段）：
 *   服务端 -> WS_TEXT 帧 -> Message_handle() 的 type=="mcp" 分支
 *   -> McpServer_ParseMessage() -> DoToolCall() -> 本文件注册的 lambda
 *   -> ReplyResult() -> SendText() -> wsock_write()
 *
 * 三条约束，修改代码前须注意：
 *
 * 1) 回调在 websocket 接收线程中同步执行（mcp_server.cpp 的 DoToolCall
 *    末尾直接 Call，未创建线程）。因此每个回调都必须是微秒级的纯内存操作。
 *    不得在回调中写 SD：一次 elmfat 写耗时数十毫秒，会阻塞整个 websocket
 *    接收循环，导致音频上行与 hello/tts 消息一并延迟。
 *    便签与闹钟的写操作全部只修改内存并置脏标志，落盘交给 app_data 的
 *    1 秒线程（见 data_store.h）。
 *
 * 2) 返回类型只有 bool、int 与 std::string（ReturnValue 的定义）。
 *    要返回多条记录只能以 std::string 拼接文本，服务端会将其作为
 *    content[0].text 交给 LLM 读取，纯文本比 JSON 更省 token。
 *
 * 3) 工具描述使用英文（现有 5 个工具均为英文，服务端据此判断是否调用）。
 *    在描述中说明"何时应使用"，比说明参数格式更能提高命中率。
 */

#include <rtthread.h>
#include <string>
#include <cstdio>
#include <cstring>

#include "mcp_server.h"

extern "C" {
#include "app_data.h"
#include "alarm.h"
#include "note.h"
#include "data_store.h"
#include "snore_rt.h"
#include "snore_log.h"
#include "lanctl.h"
}

/* 单条便签可容纳的 UTF-8 字节数（含结尾 '\0'） */
#define NOTE_TEXT_CAP   NOTE_TEXT_LEN

/* 读取一个闹钟槽的快照。
 *
 * g_app 是 volatile app_data_t，C++ 不允许 volatile 到 const 的引用绑定，
 * 逐字段拷贝是唯一可靠的方式（C 侧 memcpy 也不可行，会丢失 volatile 限定）。
 * 字段只有 4 个，手写拷贝最清晰。 */
static app_data_alarm_t alarm_snapshot(int idx)
{
    app_data_alarm_t a;

    a.hour   = g_app.alarms[idx].hour;
    a.min    = g_app.alarms[idx].min;
    a.days   = g_app.alarms[idx].days;
    a.enable = g_app.alarms[idx].enable;

    return a;
}

/* ---------------- 文本拼装小工具 ---------------- */

/* 把 uptime 秒数格式化成 "+H:MM:SS"（与鼾声页卡片一致）。 */
static std::string fmt_uptime(uint32_t sec)
{
    char buf[24];

    snprintf(buf, sizeof(buf), "+%u:%02u:%02u",
             (unsigned)(sec / 3600u),
             (unsigned)((sec / 60u) % 60u),
             (unsigned)(sec % 60u));
    return std::string(buf);
}

/* 把 days 位掩码转成人能读的形式。位序：bit0=Mon … bit6=Sun，0=Once。
 * 与 ui_alarm.c 的 alarm_days_text 保持同样的措辞，避免 UI 与 AI 说法不一。 */
static std::string fmt_days(uint8_t days)
{
    static const char *abbr[7] = { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    std::string out;
    int b;

    if (days == 0)
        return "once";
    if (days == 0x7F)
        return "daily";
    if (days == 0x1F)
        return "weekdays(Mon-Fri)";

    for (b = 0; b < 7; b++)
    {
        if (days & (1u << b))
        {
            if (!out.empty()) out += ",";
            out += abbr[b];
        }
    }
    return out.empty() ? "never" : out;
}

/* 判断 UTF-8 字节是否可以作为截断点（即该字节是一个字符的起始）。
 * 续字节的特征是高两位为 10。 */
static bool is_utf8_lead(unsigned char c)
{
    return (c & 0xC0) != 0x80;
}

/* 把 src 截断到不超过 max_bytes 字节，且不切断 UTF-8 字符。
 * 返回截断后的字节数。 */
static size_t utf8_clip(const char *src, size_t max_bytes)
{
    size_t n = strlen(src);

    if (n <= max_bytes)
        return n;

    n = max_bytes;
    /* 回退到最近的字符边界 */
    while (n > 0 && !is_utf8_lead((unsigned char)src[n]))
        n--;

    return n;
}

/* ---------------- 鼾声 ---------------- */

/* 鼾声事件。优先读 SD 上的持久化归档（跨天、跨重启），
 * 卡不可用时回退到内存里 8 条环形缓存。
 *
 * 注意回调在 ws 接收线程里跑，读文件是有代价的（几毫秒到几十毫秒）。
 * 这里只在 AI 真的问起时才读，日常对话不碰文件。 */
static ReturnValue tool_snore_events(const PropertyList &)
{
    snore_log_day_t days[7];
    int nd;

    /* 先给一张按天的汇总，这是"我最近睡得怎么样"最需要的形态 */
    nd = snore_log_list_days(days, 7);

    if (nd > 0)
    {
        std::string out = "Snore events per day (newest first), from SD archive:\n";
        int i;
        int total = 0;

        for (i = 0; i < nd; i++)
        {
            /* 256：date[16] 在编译器眼里上限 15 字符 + 不定长 %u，
             * -Wformat-truncation 按最坏情况算到 163 字节。ws 接收线程
             * 的栈上多 160 字节无所谓，换一个干净编译。 */
            char line[256];

            snprintf(line, sizeof(line), "  %s : %u events\n",
                     days[i].date, (unsigned)days[i].events);
            out += line;
            total += (int)days[i].events;
        }

        /* 今天（或最近有记录的那天）附上最新几条事件明细。
         * 只用一天：每多扫一个文件，ws 接收线程就多阻塞几十毫秒。 */
        {
            int k;

            for (k = 0; k < nd; k++)
            {
                snore_log_rec_t recs[5];
                int n;
                int j;

                if (days[k].events == 0)
                    continue;

                n = snore_log_day_tail(days[k].date, RT_NULL, recs, 5);
                if (n <= 0)
                    break;

                out += "\nLatest events on ";
                out += days[k].date;
                out += " (newest first):\n";

                for (j = 0; j < n; j++)
                {
                    rt_int64_t ts = recs[j].local_ts;
                    int sod = (ts > 0) ? (int)(ts % 86400) : 0;
                    char line[96];

                    snprintf(line, sizeof(line),
                             "  %02d:%02d:%02d  prob=%u%%  cost=%ums\n",
                             sod / 3600, (sod % 3600) / 60, sod % 60,
                             (unsigned)recs[j].prob,
                             (unsigned)recs[j].cost_ms);
                    out += line;
                }
                break;
            }
        }

        {
            char tail[96];

            snprintf(tail, sizeof(tail),
                     "total %d events over %d days. today=%u",
                     total, nd, (unsigned)snore_log_today_count());
            out += tail;
        }

        return out;
    }

    /* 没有日历时间或卡不可用：退回内存环形缓存（只覆盖本次开机） */
    {
        snore_rt_hist_t hist[SNORE_RT_HIST_MAX];
        int n = snore_rt_get_history(hist, SNORE_RT_HIST_MAX);
        int i;
        std::string out;

        if (n <= 0)
            return std::string("No snore events recorded since boot.");

        out = "Snore events since boot (no SD archive available):\n";
        for (i = 0; i < n; i++)
        {
            char line[96];

            snprintf(line, sizeof(line), "  %s  prob=%u%%  cost=%ums\n",
                     fmt_uptime(hist[i].uptime_s).c_str(),
                     (unsigned)hist[i].prob,
                     (unsigned)hist[i].cost_ms);
            out += line;
        }
        return out;
    }
}

/* 读某一天的全部事件明细。参数 date 形如 "2026-10-27"。 */
static ReturnValue tool_snore_day(const PropertyList &props)
{
    std::string date = props["date"].value<std::string>();
    snore_log_rec_t recs[24];
    int n;
    int i;
    std::string out;

    if (date.empty())
        return std::string("Missing date, expected YYYY-MM-DD.");

    n = snore_log_read_day(date.c_str(), recs, 24);
    if (n <= 0)
        return std::string("No records for " + date + ".");

    out = "Events on " + date + ":\n";
    for (i = 0; i < n; i++)
    {
        rt_int64_t ts = recs[i].local_ts;
        char line[96];

        /* 只显示时分秒：日期已经是上下文了 */
        if (ts > 0)
        {
            int sec_of_day = (int)(ts % 86400);

            snprintf(line, sizeof(line), "  %02d:%02d:%02d  prob=%u%%  cost=%ums\n",
                     sec_of_day / 3600, (sec_of_day % 3600) / 60, sec_of_day % 60,
                     (unsigned)recs[i].prob, (unsigned)recs[i].cost_ms);
        }
        else
        {
            snprintf(line, sizeof(line), "  t+%us  prob=%u%%  cost=%ums\n",
                     (unsigned)recs[i].local_ts,
                     (unsigned)recs[i].prob, (unsigned)recs[i].cost_ms);
        }
        out += line;
    }

    if (n >= 24)
        out += "(showing first 24 events)\n";

    return out;
}

static ReturnValue tool_snore_status(const PropertyList &)
{
    snore_rt_status_t st;
    char buf[192];

    snore_rt_get_status(&st);

    snprintf(buf, sizeof(buf),
             "running=%s mqtt=%s threshold=%u%% last_prob=%u%% "
             "windows=%u events=%u rms=%u",
             st.running ? "yes" : "no",
             st.mqtt_up ? "yes" : "no",
             (unsigned)st.threshold,
             (unsigned)st.last_prob,
             (unsigned)st.windows,
             (unsigned)st.events,
             (unsigned)st.rms);

    return std::string(buf);
}

/* ---------------- 闹钟 ---------------- */

static ReturnValue tool_alarm_list(const PropertyList &)
{
    std::string out = "Alarms (3 slots):\n";
    bool any = false;
    int i;

    for (i = 0; i < APP_ALARM_NUM; i++)
    {
        app_data_alarm_t a = alarm_snapshot(i);
        char line[128];

        if (!a.enable)
        {
            snprintf(line, sizeof(line), "  #%d  off\n", i);
        }
        else
        {
            any = true;
            snprintf(line, sizeof(line), "  #%d  %02d:%02d  repeat=%s  ON\n",
                     i, (int)a.hour, (int)a.min,
                     fmt_days(a.days).c_str());
        }
        out += line;
    }

    if (!any)
        out += "(none enabled)\n";

    if (!g_app.time_valid)
        out += "WARNING: device clock is not set, alarms will not fire. "
               "Connect WiFi so the time can be synced.\n";

    return out;
}

static ReturnValue tool_alarm_set(const PropertyList &properties)
{
    int slot  = properties["slot"].value<int>();
    int hour  = properties["hour"].value<int>();
    int minute = properties["minute"].value<int>();
    int days  = properties["days"].value<int>();
    bool enable = properties["enable"].value<bool>();
    app_data_alarm_t a;

    if (slot < 0 || slot >= APP_ALARM_NUM)
        return std::string("error: slot must be 0..2");

    /* 时间越界直接拒，不 clamp：AI 说"25 点"是理解错了，静默夹到 23 点
     * 会让用户以为闹钟设对了。 */
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59)
        return std::string("error: hour must be 0..23 and minute 0..59");

    if (days < 0 || days > 0x7F)
        return std::string("error: days must be 0..127 "
                           "(bit0=Mon..bit6=Sun, 0=once)");

    /* 沿用该槽原有的时分/重复日之外，只改这次传入的字段。
     * 先整体拷旧值再覆盖，避免将来 app_data_alarm_t 加字段时读到未初始化内存。 */
    a = alarm_snapshot(slot);
    a.hour   = (rt_uint8_t)hour;
    a.min    = (rt_uint8_t)minute;
    a.days   = (rt_uint8_t)days;
    a.enable = enable ? RT_TRUE : RT_FALSE;

    app_data_set_alarm(slot, &a);
    app_alarm_invalidate_slot(slot);   /* 改成当前分钟也能触发 */
    app_alarm_save_request();          /* 只置脏，1 秒线程落盘 */

    {
        char buf[160];
        snprintf(buf, sizeof(buf), "alarm #%d set to %02d:%02d repeat=%s %s",
                 slot, hour, minute, fmt_days(a.days).c_str(),
                 enable ? "(enabled)" : "(disabled)");
        return std::string(buf);
    }
}

static ReturnValue tool_alarm_clear(const PropertyList &properties)
{
    int slot = properties["slot"].value<int>();
    app_data_alarm_t a;

    if (slot < 0 || slot >= APP_ALARM_NUM)
        return std::string("error: slot must be 0..2");

    /* 只关开关，不清时分：用户下次要开时时间还在，比清空更省事 */
    a = alarm_snapshot(slot);
    a.enable = RT_FALSE;

    app_data_set_alarm(slot, &a);
    app_alarm_save_request();

    {
        char buf[96];
        snprintf(buf, sizeof(buf), "alarm #%d disabled (time kept as %02d:%02d)",
                 slot, (int)a.hour, (int)a.min);
        return std::string(buf);
    }
}

/* ---------------- 便签 ---------------- */

static ReturnValue tool_note_add(const PropertyList &properties)
{
    std::string text = properties["text"].value<std::string>();
    size_t clip;
    int slot;
    bool truncated;

    if (text.empty())
        return std::string("error: text is empty");

    /* 字数限制在此处理（Property 不支持字符串长度约束）。
     * 按 UTF-8 字符边界截断，不会切出半个汉字。 */
    clip = utf8_clip(text.c_str(), NOTE_TEXT_CAP - 1);
    truncated = (clip < text.size());
    text.resize(clip);

    slot = note_add(text.c_str());
    if (slot < 0)
    {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "error: note list is full (%d items). "
                 "Ask the user to finish or clear one first.",
                 NOTE_MAX);
        return std::string(buf);
    }

    {
        char buf[192];
        snprintf(buf, sizeof(buf), "note #%d added%s: %s",
                 slot, truncated ? " (truncated to fit)" : "", text.c_str());
        return std::string(buf);
    }
}

static ReturnValue tool_note_list(const PropertyList &)
{
    std::string out;
    int i, n = note_count();

    if (n == 0)
        return std::string("No notes.");

    out = "Notes:\n";
    for (i = 0; i < NOTE_MAX; i++)
    {
        char text[NOTE_TEXT_LEN];
        rt_bool_t done = RT_FALSE;
        char line[NOTE_TEXT_LEN + 32];

        /* 取副本而不是内部指针：note 的写方来自多个线程（UI 勾选、
         * 本文件的 note_add），锁外读内部指针会读到 memcpy 一半的串。 */
        if (!note_get_copy(i, text, sizeof(text), &done))
            continue;

        snprintf(line, sizeof(line), "  #%d [%s] %s\n",
                 i, done ? "x" : " ", text);
        out += line;
    }

    return out;
}

static ReturnValue tool_note_done(const PropertyList &properties)
{
    int index = properties["index"].value<int>();

    if (index < 0 || index >= NOTE_MAX)
        return std::string("error: index must be 0..3");

    if (note_mark(index, RT_TRUE) != 0)
        return std::string("error: no note at that index");

    {
        char buf[96];
        snprintf(buf, sizeof(buf), "note #%d marked as done", index);
        return std::string(buf);
    }
}

static ReturnValue tool_note_clear(const PropertyList &properties)
{
    /* clear=done 仅清除已完成的条目；clear=all 全部清除。
     * 默认值为 done，因为误用 all 会删除用户所有便签，不可作为默认值。 */
    std::string what = properties["which"].value<std::string>();
    int n;

    if (what == "all")
    {
        n = note_clear_all();
        return std::string("cleared all notes: " + std::to_string(n));
    }

    n = note_clear_done();
    return std::string("cleared done notes: " + std::to_string(n));
}

/* ---------------- 局域网设备（ESP32-C6 等，走 MQTT） ---------------- */

/* 等待设备回包的上限。文件头约束说回调必须是微秒级的内存操作，这里是
 * 唯一的有意例外：设备执行结果（"led on" / "unknown cmd"）对回答质量
 * 影响太大，不能只回一句"已下发"。900ms 的依据是 MQTT QoS1 在局域网
 * 内往返远小于此值；超时也只回"已下发，未等到回包"，不误报成功。
 *
 * 阻塞的代价可控：麦克风上行与 opus 编码各自有独立线程，ws 发送锁是
 * RT_WAITING_NO，因此卡住 ws 接收循环不会让上行数据积压或死锁。 */
#define LANCTL_AI_WAIT_MS   900

static ReturnValue tool_lanctl_list(const PropertyList &)
{
    std::string out;
    int n = lanctl_dev_count();
    int i;

    if (n <= 0)
        return std::string("No LAN devices are configured.");

    out = "LAN devices (MQTT):\n";
    for (i = 0; i < n; i++)
    {
        lanctl_dev_snapshot_t snap;
        char names[96];
        char line[224];

        lanctl_dev_snapshot(i, &snap);
        lanctl_dev_cmd_names(i, names, sizeof(names));

        /* seen 为真但 online 为假 = 掉线（LWT 报的 offline 或心跳超时），
         * 与"从未连上"要分开说，否则排障时会误以为设备没配好。 */
        snprintf(line, sizeof(line), "  %s (%s) %s%s%s  action=[%s]\n",
                 snap.id, snap.name,
                 snap.online ? "ONLINE" : (snap.seen ? "OFFLINE" : "NEVER SEEN"),
                 snap.ip[0] ? " ip=" : "",
                 snap.ip[0] ? snap.ip : "",
                 names[0] ? names : "-");
        out += line;

        if (snap.seen)
        {
            snprintf(line, sizeof(line), "    last reply: %s\n",
                     snap.last_reply[0] ? snap.last_reply : "-");
            out += line;
        }
    }

    if (!lanctl_mqtt_online())
        out += "WARNING: the MQTT link is currently down, "
               "commands cannot be delivered.\n";

    return out;
}

static ReturnValue tool_lanctl_send(const PropertyList &properties)
{
    std::string dev    = properties["device"].value<std::string>();
    std::string action = properties["action"].value<std::string>();
    rt_uint32_t seq = 0;
    const char *proto = RT_NULL;
    lanctl_dev_snapshot_t snap;
    char reply[96];
    int idx, rc;

    if (action.empty())
        return std::string("error: action is required");

    idx = lanctl_find_dev(dev.empty() ? RT_NULL : dev.c_str());
    if (idx < 0)
        return std::string("error: unknown device '" + dev +
                           "', call self.lanctl.list first");

    lanctl_dev_snapshot(idx, &snap);

    /* 设备不在线时直接说明，不浪费 900ms 等一个不会来的回包 */
    if (!snap.online)
        return std::string("error: device ") + snap.id +
               " is not online right now, command not sent";

    rc = lanctl_send_name(idx, action.c_str(), &seq, &proto);
    if (rc == -RT_ENOENT)
    {
        char names[96];

        lanctl_dev_cmd_names(idx, names, sizeof(names));
        return std::string("error: unknown action '") + action +
               "' for " + snap.id + ", valid actions: " + names;
    }
    if (rc == -RT_ERROR)
        return std::string("error: MQTT publish failed, device not reached");
    if (rc != 0)
        return std::string("error: cannot send command");

    rc = lanctl_wait_reply(idx, proto, seq, LANCTL_AI_WAIT_MS,
                           reply, sizeof(reply));
    if (rc == 1)
        return std::string("sent ") + action + " to " + snap.id +
               ", device replied: " + reply;

    if (rc < 0)
        return std::string("sent ") + action + " to " + snap.id +
               ", but could not read the reply";

    return std::string("sent ") + action + " to " + snap.id +
           ", no reply within " + std::to_string(LANCTL_AI_WAIT_MS) +
           "ms (device may be busy)";
}

/* ---------------- 注册 ---------------- */

extern "C" void McpServer_AddLocalTools(void);

void McpServer_AddLocalTools(void)
{
    McpServer &srv = McpServer::GetInstance();

    /* ---- 鼾声记录 ---- */
    srv.AddTool("self.snore.get_events",
                "Read snoring statistics day by day from the SD card archive. "
                "Use when the user asks how they slept, whether they snored, "
                "or how many times snoring was detected recently.",
                PropertyList(),
                tool_snore_events);

    srv.AddTool("self.snore.get_day",
                "Read the detailed snore events of one specific date "
                "(time of night, confidence, cost). Use only when the user "
                "names a particular day, e.g. 'yesterday' or 'Oct 26'.",
                PropertyList({
                    Property("date", kPropertyTypeString)
                }),
                tool_snore_day);

    srv.AddTool("self.snore.get_status",
                "Read the snore detector's current state (running, MQTT link, "
                "threshold, window and event counters, live mic level).",
                PropertyList(),
                tool_snore_status);

    /* ---- 闹钟 ---- */
    srv.AddTool("self.alarm.list",
                "List all alarm slots with their times and repeat days. "
                "Use before changing an alarm so you know which slot is free. "
                "Slots run 0..2.",
                PropertyList(),
                tool_alarm_list);

    srv.AddTool("self.alarm.set",
                "Create or update an alarm. slot: 0..2. hour: 0..23. "
                "minute: 0..59. days: bitmask where bit0=Monday .. bit6=Sunday, "
                "0 means fire only once then disable itself. "
                "enable: true to turn the alarm on. "
                "Example: tomorrow 7:30am once -> days=0; every weekday -> days=31.",
                PropertyList(
                {
                    Property("slot",   kPropertyTypeInteger, 0, 2),
                    Property("hour",   kPropertyTypeInteger, 0, 23),
                    Property("minute", kPropertyTypeInteger, 0, 59),
                    Property("days",   kPropertyTypeInteger, 0, 127),
                    Property("enable", kPropertyTypeBoolean, true),
                }),
                tool_alarm_set);

    srv.AddTool("self.alarm.clear",
                "Turn an alarm off without erasing its time. slot: 0..2.",
                PropertyList(
                {
                    Property("slot", kPropertyTypeInteger, 0, 2),
                }),
                tool_alarm_clear);

    /* ---- 便签 / 待办 ---- */
    srv.AddTool("self.note.add",
                "Add a to-do item to the device's note list (max 4 items). "
                "Keep the text short - it is displayed on a small screen and "
                "will be truncated if too long. "
                "Use when the user asks to remember or note something down.",
                PropertyList(
                {
                    Property("text", kPropertyTypeString),
                }),
                tool_note_add);

    srv.AddTool("self.note.list",
                "List the current to-do items with their indices and done flags.",
                PropertyList(),
                tool_note_list);

    srv.AddTool("self.note.done",
                "Mark a to-do item as completed. index: 0..3 (from note.list).",
                PropertyList(
                {
                    Property("index", kPropertyTypeInteger, 0, 3),
                }),
                tool_note_done);

    srv.AddTool("self.note.clear",
                "Remove to-do items. which=\"done\" clears only completed ones "
                "(default, safe); which=\"all\" clears everything.",
                PropertyList(
                {
                    Property("which", kPropertyTypeString, std::string("done")),
                }),
                tool_note_clear);

    /* ---- 局域网设备（ESP32-C6 等） ---- */
    srv.AddTool("self.lanctl.list",
                "List the LAN devices reachable over MQTT (currently an "
                "ESP32-C6 board), whether each is online, and which actions "
                "each accepts. Use before controlling one, and when the user "
                "asks whether a device is connected.",
                PropertyList(),
                tool_lanctl_list);

    srv.AddTool("self.lanctl.send",
                "Send an action to a LAN device, e.g. turn its LED on or off, "
                "ping it, or reboot it. Call self.lanctl.list first to get the "
                "device id and the valid action names. The reply text reports "
                "what the device actually did. 'reboot' takes the device down "
                "for several seconds - confirm with the user before using it "
                "unless they clearly asked to restart the device.",
                PropertyList(
                {
                    /* device 可选：只有一台设备时省略也能工作（由
                     * lanctl_find_dev 处理空串）。将来加第二台设备时，
                     * 空串会因有歧义而失败并提示模型先 list。 */
                    Property("device", kPropertyTypeString, std::string("")),
                    Property("action", kPropertyTypeString),
                }),
                tool_lanctl_send);

    rt_kprintf("[MCP] local tools registered\n");
}
