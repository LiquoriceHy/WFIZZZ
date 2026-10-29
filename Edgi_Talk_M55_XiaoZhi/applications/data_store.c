/*
 * data_store.c - 便签 + 闹钟的持久化实现
 *
 * 依赖方向：data_store 依赖 note 与 alarm 的数据接口，反向不依赖。
 * 因此 note.c 与 alarm.c 只需关心自身的内存结构，落盘细节全部集中在本文件。
 *
 * 落盘顺序沿用 app_cfg.c:468 的结论：
 *   写 tmp，stat 目标，remove 目标，rename(tmp, 目标)
 *   FatFs 的 f_rename() 在目标已存在时返回 FR_EXIST，不覆盖。
 *
 * 掉电窗口：remove 成功到 rename 完成之间若掉电，卡上会残留一个完整的
 * .tmp。data_store_load() 在 data.json 不存在时会尝试读取 .tmp，将该次
 * 修改恢复，这比 cfg 模块"只能等下次保存覆盖"更为稳妥。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "data_store.h"
#include "note.h"
#include "alarm.h"
#include "app_data.h"

#define DBG_TAG "data.store"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 去抖窗口：最后一次改动后静默达到该时长才落盘。
 *
 * 取值依据：AI 一次对话可能连发 2-4 个工具调用（add note 与 set alarm），
 * 3 秒可将它们合并为一次写；时间更长则用户可能等不到（例如设完闹钟
 * 立即断电）。app_data 的落盘循环为 1 秒一次，因此实际上界为
 * QUIET_MS 加 1s。 */
#define DATA_STORE_QUIET_MS   3000

static volatile rt_bool_t s_dirty      = RT_FALSE;
static rt_bool_t          s_quiet_started = RT_FALSE;
static rt_uint32_t        s_quiet_ms;      /* 进入安静期的时刻 */

/* ===================== JSON 工具 =====================
 * 与 app_cfg.c 的同名函数为两套独立实现（那边为 static）。
 * 不抽取公共模块的原因：两套的失败语义不同，cfg 的长度校验需要拒收，
 * 而便签只需尽力而为。强行合并会使两边都难以阅读。
 */

/* 转义：只处理 JSON 必须转义的字符，UTF-8 字节原样透传。
 * 便签内容是用户或 AI 的原始 UTF-8，不得按字符处理。 */
static int ds_escape(const char *in, char *out, rt_size_t cap)
{
    rt_size_t n = 0;

    while (*in)
    {
        unsigned char c = (unsigned char)*in;
        const char *rep = RT_NULL;
        char buf[8];

        if (c == '"')       rep = "\\\"";
        else if (c == '\\') rep = "\\\\";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        else if (c == '\t') rep = "\\t";
        else if (c < 0x20)
        {
            rt_snprintf(buf, sizeof(buf), "\\u%04x", c);
            rep = buf;
        }

        if (rep != RT_NULL)
        {
            rt_size_t rl = rt_strlen(rep);

            if (n + rl >= cap) return -1;
            rt_memcpy(out + n, rep, rl);
            n += rl;
        }
        else
        {
            if (n + 1 >= cap) return -1;
            out[n++] = (char)c;
        }
        in++;
    }

    out[n] = '\0';
    return (int)n;
}

/* 找 "key" 对应的字符串值，写入 out。返回 0 找到，-1 没找到。
 *
 * 只处理扁平 {"k":"v"}：跳过空白，逐个读取 "key" : "value" 对。
 * 与 cfg 的同名函数逻辑一致，但不做 \u 解码，因为便签中不会出现由本模块
 * 写入的 \u 转义（仅控制字符会产生转义，而那些字符对便签无意义，
 * 保持原样即可）。 */
static int ds_get_string(const char *json, const char *key, char *out, rt_size_t cap)
{
    const char *p = json;
    rt_size_t klen = rt_strlen(key);

    while (*p)
    {
        /* 找下一个 '"'，读 key */
        while (*p && *p != '"') p++;
        if (*p == '\0') return -1;
        p++;

        if (rt_strncmp(p, key, klen) == 0 && p[klen] == '"')
        {
            const char *v;
            rt_size_t n = 0;

            p += klen + 1;
            /* 跳过 : 与空白 */
            while (*p && (*p == ':' || *p == ' ' || *p == '\t' ||
                          *p == '\n' || *p == '\r')) p++;
            if (*p != '"') return -1;
            p++;

            v = p;
            while (*v && *v != '"')
            {
                char ch = *v;

                if (ch == '\\' && v[1] != '\0')
                {
                    v++;
                    switch (*v)
                    {
                    case 'n':  ch = '\n'; break;
                    case 'r':  ch = '\r'; break;
                    case 't':  ch = '\t'; break;
                    case '"':  ch = '"';  break;
                    case '\\': ch = '\\'; break;
                    default:   ch = *v;   break;
                    }
                }

                if (n + 1 < cap) out[n++] = ch;
                v++;
            }
            out[n] = '\0';
            return 0;
        }

        /* 不是目标 key：跳过这个字符串 */
        while (*p && *p != '"') p++;
        if (*p == '\0') return -1;
        p++;
    }

    return -1;
}

/* ===================== 读盘 ===================== */

/* 读整个文件到堆。失败返回 RT_NULL。上限 8KB：4 条便签 ×64B + 3 个闹钟
 * 满载也才约 1KB，超了就是损坏。 */
static char *ds_read_file(const char *path)
{
    FILE *fp;
    long sz;
    char *buf;

    fp = fopen(path, "r");
    if (fp == RT_NULL) return RT_NULL;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return RT_NULL; }
    sz = ftell(fp);
    if (sz <= 0 || sz > 8192) { fclose(fp); return RT_NULL; }
    rewind(fp);

    buf = (char *)rt_malloc((rt_size_t)sz + 1);
    if (buf == RT_NULL) { fclose(fp); return RT_NULL; }

    if (fread(buf, 1, (rt_size_t)sz, fp) != (rt_size_t)sz)
    {
        rt_free(buf);
        fclose(fp);
        return RT_NULL;
    }
    buf[sz] = '\0';
    fclose(fp);

    return buf;
}

/* 从一段 JSON 中恢复便签与闹钟。
 *
 * 先全部解析到临时数组，全部成功后才提交到 note/alarms。
 * 不能边解析边写入：文件损坏时（例如写到一半掉电）解析会中途失败，
 * 此时若已调用 note_reset 并写入若干条，用户原有的便签即被清除，
 * 而"先清空再加载"的顺序会使开机路径上的"读取失败"演变为"数据丢失"。 */
static int ds_apply(const char *json)
{
    char notes[NOTE_MAX][NOTE_TEXT_LEN];
    rt_bool_t done_flags[NOTE_MAX];
    app_data_alarm_t alarms[APP_ALARM_NUM];
    rt_bool_t alarm_ok[APP_ALARM_NUM];
    char num[8];
    char key[32];
    int i;
    int applied = 0;

    for (i = 0; i < NOTE_MAX; i++)
    {
        notes[i][0] = '\0';
        done_flags[i] = RT_FALSE;
    }
    for (i = 0; i < APP_ALARM_NUM; i++)
    {
        rt_memset(&alarms[i], 0, sizeof(alarms[i]));
        alarm_ok[i] = RT_FALSE;
    }

    /* ---- 解析便签（纯内存，不碰 note 模块） ---- */
    for (i = 0; i < NOTE_MAX; i++)
    {
        rt_snprintf(key, sizeof(key), "note%d", i);
        if (ds_get_string(json, key, notes[i], NOTE_TEXT_LEN) != 0)
            continue;

        rt_snprintf(key, sizeof(key), "note%d_done", i);
        if (ds_get_string(json, key, num, sizeof(num)) != 0)
            num[0] = '\0';

        done_flags[i] = (atoi(num) != 0) ? RT_TRUE : RT_FALSE;
        applied++;
    }

    /* ---- 解析闹钟 ---- */
    for (i = 0; i < APP_ALARM_NUM; i++)
    {
        app_data_alarm_t a;
        char h[8], m[8], d[8], e[8];

        rt_snprintf(key, sizeof(key), "alarm%d_h", i);
        if (ds_get_string(json, key, h, sizeof(h)) != 0) continue;
        rt_snprintf(key, sizeof(key), "alarm%d_m", i);
        if (ds_get_string(json, key, m, sizeof(m)) != 0) continue;
        rt_snprintf(key, sizeof(key), "alarm%d_days", i);
        if (ds_get_string(json, key, d, sizeof(d)) != 0) continue;
        rt_snprintf(key, sizeof(key), "alarm%d_en", i);
        if (ds_get_string(json, key, e, sizeof(e)) != 0) continue;

        a.hour   = (rt_uint8_t)atoi(h);
        a.min    = (rt_uint8_t)atoi(m);
        a.days   = (rt_uint8_t)atoi(d);
        a.enable = (atoi(e) != 0) ? RT_TRUE : RT_FALSE;

        /* 越界值一律当损坏，跳过这一槽（保留默认的关闭状态） */
        if (a.hour > 23 || a.min > 59) continue;

        alarms[i] = a;
        alarm_ok[i] = RT_TRUE;
    }

    /* ---- 提交：到这里解析全部结束，失败不会再影响已有数据 ---- */
    for (i = 0; i < NOTE_MAX; i++)
    {
        if (notes[i][0] != '\0' || done_flags[i])
            note_restore(i, notes[i], done_flags[i]);
    }

    for (i = 0; i < APP_ALARM_NUM; i++)
    {
        if (!alarm_ok[i]) continue;

        /* 使用 setter 而非整个结构体赋值。
         *
         * 本函数运行在 data_store_boot_task 线程中，而 LVGL 线程（卡片 1 秒
         * timer 读取 g_app.alarms）与 alarm 线程（每秒比对）此时已在运行，
         * 因此"开机早期无人读取"的判断并不成立。setter 的写入顺序（先数值
         * 后 enable）保证读者看到 enable=1 时数值已完整，不会读到不完整的
         * 闹钟配置。 */
        app_data_set_alarm(i, &alarms[i]);
        applied++;
    }

    return applied;
}

int data_store_load(void)
{
    char *json;
    int n;

    json = ds_read_file(DATA_STORE_FILE);

    /* 主文件不存在时尝试恢复 .tmp：这是上次 remove 与 rename 之间掉电的
     * 残留文件，内容完整（写 tmp 时已 fclose 刷盘）。 */
    if (json == RT_NULL)
    {
        json = ds_read_file(DATA_STORE_FILE_TMP);
        if (json != RT_NULL)
            LOG_W("%s missing, recovered from .tmp", DATA_STORE_FILE);
    }

    if (json == RT_NULL)
    {
        LOG_I("no %s yet, notes/alarms start empty", DATA_STORE_FILE);
        return -RT_ENOENT;
    }

    n = ds_apply(json);
    rt_free(json);

    LOG_I("loaded %d note(s)/alarm(s) from %s", n, DATA_STORE_FILE);
    return (n > 0) ? 0 : -RT_EINVAL;
}

/* ===================== 写盘 ===================== */

/* 写一个 "key": "value" 行。last 为真时不加逗号。
 *
 * 逗号不能像 cfg 模块那样以"末尾补一条假键"收尾，因为那会在文件中留下
 * 一个无语义的 "_v"，后续按格式修改时容易出错。
 *
 * 转义缓冲使用 static 而非栈：本函数由 data_store_save 经 app_data 的
 * 1 秒线程调用，该线程栈仅 2KB，而此处需要 392 字节（64 × 6 + 8）。
 * 单线程使用（落盘仅在 data_store_poll 中发生），static 是安全的。 */
static int ds_put_str(FILE *fp, const char *key, const char *val, int last)
{
    static char esc[NOTE_TEXT_LEN * 6 + 8];

    if (ds_escape(val, esc, sizeof(esc)) < 0)
        return -RT_EINVAL;

    if (fprintf(fp, "  \"%s\": \"%s\"%s\n", key, esc, last ? "" : ",") < 0)
        return -RT_EIO;

    return 0;
}

static int ds_write_file(const char *path)
{
    FILE *fp;
    int ret = 0;
    int i;
    char key[32];
    char flag[4];

    fp = fopen(path, "w");
    if (fp == RT_NULL)
    {
        LOG_W("open %s for write failed", path);
        return -RT_EIO;
    }

    /* 每个 fprintf 都查返回：卡写满时它会返回负数，而 FILE 缓冲里
     * 可能还留着部分内容，fclose 也可能"成功"，最终得到一个字段缺失
     * 却判定写成功的文件。 */
    if (fprintf(fp, "{\n") < 0) { ret = -RT_EIO; goto __close; }

    /* ---- 便签 ---- */
    for (i = 0; i < NOTE_MAX; i++)
    {
        char text[NOTE_TEXT_LEN];
        rt_bool_t done = RT_FALSE;

        if (!note_get_copy(i, text, sizeof(text), &done))
            continue;

        rt_snprintf(key, sizeof(key), "note%d", i);
        ret = ds_put_str(fp, key, text, 0);
        if (ret != 0) goto __close;

        rt_snprintf(flag, sizeof(flag), "%d", done ? 1 : 0);
        rt_snprintf(key, sizeof(key), "note%d_done", i);
        ret = ds_put_str(fp, key, flag, 0);
        if (ret != 0) goto __close;
    }

    /* ---- 闹钟 ---- */
    for (i = 0; i < APP_ALARM_NUM; i++)
    {
        /* g_app 是 volatile：不能直接取地址赋给普通指针。
         * 逐个字段读，与 mcp_local_tools.cpp 的 alarm_snapshot 同理。 */
        rt_uint8_t ah = g_app.alarms[i].hour;
        rt_uint8_t am = g_app.alarms[i].min;
        rt_uint8_t ad = g_app.alarms[i].days;
        rt_bool_t  ae = g_app.alarms[i].enable;
        char num[8];
        int last = (i + 1 == APP_ALARM_NUM);

        rt_snprintf(num, sizeof(num), "%d", (int)ah);
        rt_snprintf(key, sizeof(key), "alarm%d_h", i);
        ret = ds_put_str(fp, key, num, 0);
        if (ret != 0) goto __close;

        rt_snprintf(num, sizeof(num), "%d", (int)am);
        rt_snprintf(key, sizeof(key), "alarm%d_m", i);
        ret = ds_put_str(fp, key, num, 0);
        if (ret != 0) goto __close;

        rt_snprintf(num, sizeof(num), "%d", (int)ad);
        rt_snprintf(key, sizeof(key), "alarm%d_days", i);
        ret = ds_put_str(fp, key, num, 0);
        if (ret != 0) goto __close;

        rt_snprintf(num, sizeof(num), "%d", ae ? 1 : 0);
        rt_snprintf(key, sizeof(key), "alarm%d_en", i);
        ret = ds_put_str(fp, key, num, last);
        if (ret != 0) goto __close;
    }

    if (fprintf(fp, "}\n") < 0) ret = -RT_EIO;

__close:
    if (fclose(fp) != 0 && ret == 0)
    {
        LOG_W("flush %s failed", path);
        ret = -RT_EIO;
    }

    if (ret != 0) LOG_W("write %s failed (%d)", path, ret);
    return ret;
}

int data_store_save(void)
{
    struct stat st;
    int ret;

    /* 复用 app_cfg 的挂载点判断：该函数依据的是 /sdcard 而非工作目录。
     * 此处只 stat 一次，不做写探针，因为写探针已在 app_cfg 初始化时执行，
     * 且本函数由 1 秒线程调用，不应每次都额外写一个文件。 */
    if (stat("/sdcard", &st) != 0 || !S_ISDIR(st.st_mode))
    {
        LOG_W("SD not mounted, data stays RAM-only");
        return -RT_EIO;
    }

    ret = ds_write_file(DATA_STORE_FILE_TMP);
    if (ret != 0) return ret;

    /* 目标已存在必须先删：FatFs 的 rename 不覆盖（app_cfg.c:485 的实证）。 */
    if (stat(DATA_STORE_FILE, &st) == 0)
    {
        if (remove(DATA_STORE_FILE) != 0)
        {
            LOG_W("remove old %s failed", DATA_STORE_FILE);
            remove(DATA_STORE_FILE_TMP);
            return -RT_EIO;
        }
    }

    if (rename(DATA_STORE_FILE_TMP, DATA_STORE_FILE) != 0)
    {
        LOG_W("rename %s -> %s failed", DATA_STORE_FILE_TMP, DATA_STORE_FILE);
        remove(DATA_STORE_FILE_TMP);
        return -RT_EIO;
    }

    s_dirty = RT_FALSE;
    s_quiet_started = RT_FALSE;

    LOG_I("notes/alarms saved to %s", DATA_STORE_FILE);
    return 0;
}

/* ===================== 脏标志与去抖 ===================== */

void data_store_mark_dirty(void)
{
    if (!s_dirty)
    {
        s_dirty = RT_TRUE;
    }
    /* 每次改动都把安静计时清零：连续 add 只在最后一次之后写盘 */
    s_quiet_started = RT_TRUE;
    s_quiet_ms = rt_tick_get_millisecond();
}

rt_bool_t data_store_is_dirty(void)
{
    return s_dirty;
}

void data_store_poll(void)
{
    rt_uint32_t now;

    if (!s_dirty) return;
    if (!s_quiet_started) return;

    now = rt_tick_get_millisecond();

    /* 无符号减法天然处理计数回绕（rt_tick_get_millisecond 是 32 位） */
    if ((rt_uint32_t)(now - s_quiet_ms) < DATA_STORE_QUIET_MS)
        return;

    /* 落盘失败时保留 dirty，等下一次 poll 重试。
     * 不在这里 mdelay 重试：本函数跑在 1 秒循环里，卡死会拖垮整个 app_data
     * 线程（它还负责时钟、电量、WiFi 状态）。 */
    (void)data_store_save();
}

/* ===================== 开机加载 ===================== */

/* 使用独立线程而非在 INIT_APP_EXPORT 中直接读取的原因：
 * SD 挂载由 mnt.c 的热插拔线程异步完成（每 1 秒轮询），开机瞬间通常
 * 尚未就绪。在导出回调中 mdelay 等待会阻塞 main 线程，使 LVGL 首屏与
 * 按键响应一同延后。app_cfg 模块存在同样情形，采用相同解法。
 *
 * 优先级 26、栈 2KB：属于补充加载线程，延迟可接受；调用链只有 stat 与 fopen。
 * 等待 10 秒与 app_cfg_boot_task 保持一致，对应同一个 SD 挂载完成时刻。 */
#define DS_BOOT_STACK   2048
#define DS_BOOT_PRIO    26
#define DS_BOOT_WAIT_MS 10000

static void data_store_boot_task(void *parameter)
{
    struct stat st;
    int waited = 0;

    RT_UNUSED(parameter);

    while (waited < DS_BOOT_WAIT_MS)
    {
        if (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode))
        {
            /* note_init 由 ds_apply 内部调（先清空再填），这里只读 */
            (void)data_store_load();
            /* 读回来的数据不该被当成"待落盘改动" */
            s_dirty = RT_FALSE;
            s_quiet_started = RT_FALSE;
            return;
        }

        rt_thread_mdelay(500);
        waited += 500;
    }

    LOG_W("no SD after %d ms: notes/alarms stay RAM-only", waited);
}

static int data_store_boot_start(void)
{
    rt_thread_t tid;

    tid = rt_thread_create("databoot", data_store_boot_task, RT_NULL,
                           DS_BOOT_STACK, DS_BOOT_PRIO, 10);
    if (tid != RT_NULL) rt_thread_startup(tid);

    return RT_EOK;
}
INIT_APP_EXPORT(data_store_boot_start);
