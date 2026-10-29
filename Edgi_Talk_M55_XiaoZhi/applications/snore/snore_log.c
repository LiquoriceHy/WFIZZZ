/*
 * snore_log.c - 鼾声事件按天归档到 SD
 *
 * 线程模型：
 *   snore_log_start 创建一个低优先级线程与一个定长环形队列。
 *   推理线程调用 snore_log_append() 时仅在临界区内入队（微秒级）即返回，
 *   实际的 open/write/close 全部在归档线程中执行。
 *
 * 不在推理线程中直接写盘的原因：SD 追加写在 FatFs 上包含目录项更新，
 * 单次耗时数十毫秒。推理一窗为 337ms，余量仅 2.9 倍，同步写会使时序
 * 变得不确定。队列 16 条足以吸收抖动（事件冷却 15s，正常情况下不会积压）。
 *
 * 掉电语义：追加写没有 read-modify-write，掉电最多丢失最后一行。
 * 代价是文件可能残留半行，读取侧按"解析失败即跳过"处理。
 */
#include <rtthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "snore_log.h"
#include "app_data.h"      /* g_app.time_ts / time_valid */

#define DBG_TAG "snore.log"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define SL_THREAD_STACK   4096
#define SL_THREAD_PRIO    28        /* 低于推理线程(24)，不占用推理所需 CPU */
#define SL_THREAD_TICK    20

#define SL_LINE_MAX       48        /* "1793088000,93,412\n" 远小于此 */

/* ===================== 状态 ===================== */

static snore_log_rec_t s_queue[SNORE_LOG_QUEUE_LEN];
static rt_uint8_t      s_q_head;    /* 下一个写入位置 */
static rt_uint8_t      s_q_count;
static struct rt_mutex s_q_lock;    /* 静态初始化，避免堆分配失败 */
static rt_bool_t       s_q_lock_ready = RT_FALSE;

static volatile rt_bool_t  s_run = RT_FALSE;
static volatile rt_bool_t  s_exit = RT_FALSE;
static rt_thread_t         s_thread = RT_NULL;
static struct rt_semaphore s_wake;        /* 有新记录时唤醒归档线程 */
static rt_bool_t           s_sem_ready = RT_FALSE;

static volatile rt_uint32_t s_today_events = 0;
static volatile rt_uint32_t s_dropped = 0;
static char        s_today_date[16] = "";   /* "2026-10-27" */
static rt_bool_t   s_dir_ready = RT_FALSE;

/* ===================== 日期工具 ===================== */

/* Howard Hinnant 的 civil_from_days：将 Unix 天数转换为 y/m/d。
 * 不使用 time.h 的 gmtime/localtime，因为本工程未启用 LIBC 的完整时间支持，
 * 且此处只需要"天"这一级精度。 */
static void sl_civil_from_days(long z, int *y, int *m, int *d)
{
    long era, doe, yoe, doy, mp;
    unsigned y2, mm, dd;

    z += 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y2  = (unsigned)(yoe + era * 400);
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    dd  = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    mm  = (unsigned)(mp < 10 ? mp + 3 : mp - 9);

    *y = (int)(y2 + (mm <= 2 ? 1 : 0));
    *m = (int)mm;
    *d = (int)dd;
}

/* 本地时间戳 → "YYYY-MM-DD"。ts<=0 返回 "unknown"。
 * 对外暴露为 snore_log_date_of（见头文件），供分页栏算日期用。 */
void snore_log_date_of(rt_int64_t ts, char *out, rt_size_t len)
{
    long days;
    int y, m, d;

    if (ts <= 0)
    {
        rt_snprintf(out, len, "unknown");
        return;
    }

    days = (long)(ts / 86400);
    /* 负数时间戳向 -inf 取整，避免 1970 前的边界错一天 */
    if (ts < 0 && (ts % 86400) != 0)
        days--;
    sl_civil_from_days(days, &y, &m, &d);
    rt_snprintf(out, len, "%04d-%02d-%02d", y, m, d);
}

/* ===================== 文件路径 ===================== */

static rt_uint32_t sl_count_file(const char *path);   /* 前置声明，见读取段 */

static void sl_path_of(const char *date, char *out, rt_size_t len)
{
    rt_snprintf(out, len, SNORE_LOG_DIR "/%s.csv", date);
}

/* 建目录（elmfat 的 mkdir 不递归，逐级来）。
 * 只建两级：/sdcard 已由挂载提供，/sdcard/wfizzz 可能已被 cfg 建好。 */
static rt_bool_t sl_ensure_dir(void)
{
    struct stat st;

    if (s_dir_ready)
        return RT_TRUE;

    if (stat("/sdcard", &st) != 0 || !S_ISDIR(st.st_mode))
        return RT_FALSE;

    if (stat("/sdcard/wfizzz", &st) != 0)
        mkdir("/sdcard/wfizzz", 0);

    if (stat(SNORE_LOG_DIR, &st) != 0)
    {
        if (mkdir(SNORE_LOG_DIR, 0) != 0)
        {
            LOG_W("mkdir %s failed", SNORE_LOG_DIR);
            return RT_FALSE;
        }
    }

    s_dir_ready = RT_TRUE;
    return RT_TRUE;
}

/* ===================== 写文件 ===================== */

/* 追加一条。调用方已保证在归档线程里（单线程访问，无需锁）。 */
static void sl_write_one(const snore_log_rec_t *r)
{
    char date[16];
    char path[64];
    rt_int64_t ts_key;
    FILE *fp;

    /* 判据用 ts_valid（append 时快照的 g_app.time_valid），不用 local_ts > 0。
     * 软 RTC 从 2018-01-01 起算，未校时期间 time_ts 也恒为正，只看符号会把
     * 事件写进 2018-01-0X.csv 这种看似合法但完全错误的文件，而 unknown.csv
     * 兜底分支形同虚设。 */
    if (r->ts_valid && r->local_ts > 0)
    {
        ts_key = r->local_ts;
        snore_log_date_of(r->local_ts, date, sizeof(date));
    }
    else
    {
        ts_key = (rt_int64_t)r->uptime_s;
        rt_snprintf(date, sizeof(date), "unknown");
    }

    if (!sl_ensure_dir())
        return;

    sl_path_of(date, path, sizeof(path));

    /* 用 "a"（追加）而不是 "w"：文件里已有更早的记录，
     * 每次重写整文件既慢又会在掉电时丢掉全部历史。 */
    fp = fopen(path, "a");
    if (fp == RT_NULL)
    {
        LOG_W("open %s failed", path);
        return;
    }

    /* 使用 %ld 而非 %lld：本工程链接 nano.specs（newlib-nano），其
     * _svfprintf_r 没有 long long 分支（反汇编确认：函数体内无
     * __aeabi_ldivmod 调用）。使用 %lld 不会报错，而是静默输出错误内容，
     * 该问题比编译错误更难排查。
     *
     * 时间戳使用 32 位：Unix 秒到 2038 年才溢出，而本地时间基数经
     * AD_TZ_OFFSET_SEC 折算，2038 之前均安全。若届时需要处理，
     * 应更换整个时间子系统，而非仅修改此处。 */
    fprintf(fp, "%ld,%u,%u\n",
            (long)ts_key,
            (unsigned)r->prob,
            (unsigned)r->cost_ms);
    fclose(fp);

    /* 跨天检测：日期变了就重置当日计数。
     *
     * 未校时的事件落 unknown.csv，不属于任何一天，不能参与这个计数——
     * 否则一条开机早期的事件会把 date 变成 "unknown"，把当天的真实累计
     * 清成 0，UI 和 AI 看到的"今日次数"就凭空变少了。 */
    if (!r->ts_valid)
        return;

    if (rt_strcmp(date, s_today_date) != 0)
    {
        rt_strncpy(s_today_date, date, sizeof(s_today_date) - 1);
        s_today_date[sizeof(s_today_date) - 1] = '\0';
        s_today_events = 0;
    }
    s_today_events++;
}

/* ===================== 归档线程 ===================== */

static void sl_thread_entry(void *p)
{
    RT_UNUSED(p);

    while (!s_exit)
    {
        snore_log_rec_t rec;

        if (rt_sem_take(&s_wake, rt_tick_from_millisecond(500)) != RT_EOK)
            continue;

        /* 一次唤醒把队列排空，减少往返 */
        for (;;)
        {
            rt_bool_t got = RT_FALSE;

            if (s_q_lock_ready)
                rt_mutex_take(&s_q_lock, RT_WAITING_FOREVER);

            if (s_q_count > 0)
            {
                rt_uint8_t tail = (rt_uint8_t)((s_q_head + SNORE_LOG_QUEUE_LEN
                                                - s_q_count) % SNORE_LOG_QUEUE_LEN);

                rec = s_queue[tail];
                s_q_count--;
                got = RT_TRUE;
            }

            if (s_q_lock_ready)
                rt_mutex_release(&s_q_lock);

            if (!got)
                break;

            sl_write_one(&rec);
        }
    }

    s_run = RT_FALSE;
    s_thread = RT_NULL;
}

/* ===================== 对外接口 ===================== */

static void sl_restore_today(void);   /* 前置声明，见下方计数段 */

void snore_log_start(void)
{
    if (s_thread != RT_NULL)
        return;

    if (!s_q_lock_ready)
    {
        if (rt_mutex_init(&s_q_lock, "slog", RT_IPC_FLAG_FIFO) == RT_EOK)
            s_q_lock_ready = RT_TRUE;
    }

    if (!s_sem_ready)
    {
        if (rt_sem_init(&s_wake, "slog", 0, RT_IPC_FLAG_FIFO) == RT_EOK)
            s_sem_ready = RT_TRUE;
    }

    s_exit = RT_FALSE;
    s_run  = RT_TRUE;

    /* 恢复当日计数要在起线程之前做：线程一起来就可能开始写，
     * 那时 s_today_events 还该是"文件里已有的条数"。 */
    sl_restore_today();

    s_thread = rt_thread_create("snore_log", sl_thread_entry, RT_NULL,
                                SL_THREAD_STACK, SL_THREAD_PRIO, SL_THREAD_TICK);
    if (s_thread == RT_NULL)
    {
        LOG_E("create thread failed");
        s_run = RT_FALSE;
        return;
    }
    rt_thread_startup(s_thread);
    LOG_I("archiver started");
}

void snore_log_stop(void)
{
    if (s_thread == RT_NULL)
        return;

    /* 先把队列写干净再退出：置 exit 后线程会跳出主循环，
     * 所以这里等它自己处理完当前批次。 */
    s_exit = RT_TRUE;
    if (s_sem_ready)
        rt_sem_release(&s_wake);

    {
        int waited = 0;

        while (s_thread != RT_NULL && waited < 3000)
        {
            rt_thread_mdelay(20);
            waited += 20;
        }
        if (s_thread != RT_NULL)
            LOG_W("thread did not exit in 3s");
    }

    s_exit = RT_FALSE;
    LOG_I("archiver stopped");
}

void snore_log_append(rt_uint16_t prob, rt_uint32_t cost_ms)
{
    snore_log_rec_t *r;

    if (!s_run || !s_q_lock_ready)
        return;

    rt_mutex_take(&s_q_lock, RT_WAITING_FOREVER);

    if (s_q_count >= SNORE_LOG_QUEUE_LEN)
    {
        rt_mutex_release(&s_q_lock);
        s_dropped++;
        LOG_W("queue full, event dropped");
        return;
    }

    r = &s_queue[s_q_head];
    /* 时间在这里取快照：归档线程可能晚几百毫秒才处理，
     * 但事件发生的时刻才是要记录的。
     *
     * ts_valid 也必须一起快照：若归档线程处理时 NTP 刚好校时成功，
     * 重新判断 time_valid 会得到"真"，把一条发生在未校时期间的事件
     * 错记到校时后的日期文件里。local_ts 与 ts_valid 必须来自同一时刻。 */
    r->local_ts = g_app.time_ts;
    r->ts_valid = g_app.time_valid;
    r->uptime_s = (rt_uint32_t)(rt_tick_get() / RT_TICK_PER_SECOND);
    r->prob     = prob;
    r->cost_ms  = cost_ms;

    s_q_head = (rt_uint8_t)((s_q_head + 1) % SNORE_LOG_QUEUE_LEN);
    s_q_count++;

    rt_mutex_release(&s_q_lock);

    if (s_sem_ready)
        rt_sem_release(&s_wake);
}

rt_uint32_t snore_log_today_count(void)
{
    return s_today_events;
}

/* 开机恢复：从今天的文件里数出已有条数，否则重启后 UI/AI 会看到
 * "今日 0 次"，而同一天早先记录明明在卡上。由 snore_log_start 调用。 */
static void sl_restore_today(void)
{
    char date[16];
    char path[64];

    if (!g_app.time_valid)
        return;

    snore_log_date_of(g_app.time_ts, date, sizeof(date));
    rt_strncpy(s_today_date, date, sizeof(s_today_date) - 1);
    s_today_date[sizeof(s_today_date) - 1] = '\0';

    sl_path_of(date, path, sizeof(path));
    s_today_events = sl_count_file(path);
    if (s_today_events)
        LOG_I("restored today=%s count=%u", date, (unsigned)s_today_events);
}

rt_uint32_t snore_log_dropped(void)
{
    return s_dropped;
}

/* ===================== 读取 ===================== */

/* 数一个文件里有效行数（跳过 '#' 注释和解析失败的行） */
static rt_uint32_t sl_count_file(const char *path)
{
    char line[SL_LINE_MAX];
    FILE *fp;
    rt_uint32_t n = 0;

    fp = fopen(path, "r");
    if (fp == RT_NULL)
        return 0;

    while (fgets(line, sizeof(line), fp) != RT_NULL)
    {
        if (line[0] == '#')
            continue;
        /* 至少要有一个逗号才算合法行 */
        if (strchr(line, ',') != RT_NULL)
            n++;
    }
    fclose(fp);
    return n;
}

int snore_log_list_days(snore_log_day_t *buf, int max)
{
    int n = 0;
    rt_int64_t ts;
    int i;

    if (buf == RT_NULL || max <= 0)
        return 0;

    if (g_app.time_valid)
        ts = g_app.time_ts;
    else
        return 0;   /* 没有日历时间就无法按天枚举，交给调用方回退 */

    /* 从今天往前扫 max 天。不做目录遍历：DFS v1 的 readdir 在
     * elmfat 上对大目录逐个 stat 很慢，而这里只需要"最近 N 天"。 */
    for (i = 0; i < max; i++)
    {
        rt_int64_t day_ts = ts - (rt_int64_t)i * 86400;
        char path[64];

        /* 取当天正午再转日期，避开边界抖动：若 ts 恰好落在午夜附近，
         * 直接用 day_ts 可能把相邻两天算成同一天。 */
        snore_log_date_of(day_ts - (day_ts % 86400) + 12 * 3600,
                   buf[n].date, sizeof(buf[n].date));
        sl_path_of(buf[n].date, path, sizeof(path));
        buf[n].events = sl_count_file(path);
        n++;
    }

    return n;
}

int snore_log_read_day(const char *date, snore_log_rec_t *buf, int max)
{
    char path[64];
    char line[SL_LINE_MAX];
    FILE *fp;
    int n = 0;

    if (date == RT_NULL)
        return 0;

    sl_path_of(date, path, sizeof(path));
    fp = fopen(path, "r");
    if (fp == RT_NULL)
        return 0;

    while (fgets(line, sizeof(line), fp) != RT_NULL)
    {
        long ts;
        unsigned prob, cost;

        if (line[0] == '#')
            continue;
        if (strchr(line, ',') == RT_NULL)
            continue;

        if (buf != RT_NULL && n < max)
        {
            /* 解析失败（掉电留下的半行）就当没有。
             * 同样用 %ld，理由见 sl_write_one。 */
            if (sscanf(line, "%ld,%u,%u", &ts, &prob, &cost) == 3)
            {
                buf[n].local_ts = (rt_int64_t)ts;
                buf[n].uptime_s = 0;
                buf[n].prob     = (rt_uint16_t)prob;
                buf[n].cost_ms  = (rt_uint16_t)cost;
                n++;
            }
        }
        else if (buf == RT_NULL)
        {
            /* 只要计数：上面的逗号检查已经确认是有效行 */
            n++;
        }
        else
        {
            break;
        }
    }
    fclose(fp);
    return n;
}

/* 一遍扫描：算总数 + 取最新 max 条。
 *
 * 用环形缓冲（而不是"全读再截尾"或"先数再 seek"）：
 *   - 全读：上限 5760 条 × 16 字节 = 92KB，主堆余量只有约 40KB，放不下。
 *   - 先数再 seek：要扫两遍文件，SD 上翻倍。
 *   - 环形缓冲：单遍、内存只与 max 成正比（6 条 = 96 字节）。
 *
 * 写侧是追加写，文件按时间升序，所以环形缓冲里最后写入的就是最新的一条。
 * 读完后把环形缓冲按"最新在前"展开到 buf。 */
int snore_log_day_tail(const char *date, rt_uint32_t *total,
                       snore_log_rec_t *buf, int max)
{
    char path[64];
    char line[SL_LINE_MAX];
    FILE *fp;
    rt_uint32_t n_total = 0;
    int head = 0;       /* 下一个写入位置 */
    int filled = 0;     /* 环形缓冲里已有条数 */

    if (date == RT_NULL)
    {
        if (total != RT_NULL)
            *total = 0;
        return 0;
    }
    if (max < 0)
        max = 0;

    sl_path_of(date, path, sizeof(path));
    fp = fopen(path, "r");
    if (fp == RT_NULL)
    {
        if (total != RT_NULL)
            *total = 0;
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != RT_NULL)
    {
        long ts;
        unsigned prob, cost;

        if (line[0] == '#')
            continue;
        if (strchr(line, ',') == RT_NULL)
            continue;
        /* 与 read_day 同口径：sscanf 失败（掉电半行）跳过，不计入总数。
         * 口径必须一致，否则翻页栏的"TOTAL"和实际能翻到的条数对不上。 */
        if (sscanf(line, "%ld,%u,%u", &ts, &prob, &cost) != 3)
            continue;

        n_total++;

        if (buf == RT_NULL || max == 0)
            continue;

        buf[head].local_ts = (rt_int64_t)ts;
        buf[head].uptime_s = 0;
        buf[head].prob     = (rt_uint16_t)(prob > 100 ? 100 : prob);
        buf[head].cost_ms  = (rt_uint16_t)(cost > 65535 ? 65535 : cost);
        buf[head].ts_valid = RT_TRUE;

        head = (head + 1) % max;
        if (filled < max)
            filled++;
    }
    fclose(fp);

    if (total != RT_NULL)
        *total = n_total;

    if (buf == RT_NULL || filled == 0)
        return 0;

    /* 环形缓冲 → 输出数组，最新在前。
     *
     * 不能就地重排：源索引从 head-1 递减、目标索引从 0 递增，两者会交叉，
     * 搬到中间时一半源数据已被覆盖。所以借一份临时空间当源。
     *
     * 用 static 而不是栈数组：本函数在 ws 接收线程（AI 工具回调）里被
     * 调用，那条线程还背着 mbedTLS 的记录缓冲，栈不宽裕。static 安全
     * 的前提是"同一条线程调用"——见下方注释。
     *
     * 上限 32：超出时不再静默跳过（早期版本 `if (filled <= 16)` 否则
     * 整段不重排，会返回环形顺序而非最新在前，是个静默的错误结果）。
     * 现在超限就截断到 32 条，仍然保证顺序正确。 */
    {
        static snore_log_rec_t tmp[32];
        int keep = (filled < 32) ? filled : 32;
        int base = (head + max - keep) % max;   /* 最旧一条的位置 */
        int k;

        for (k = 0; k < keep; k++)
            tmp[k] = buf[(base + k) % max];

        /* tmp 是升序（旧→新），倒着写出来就是最新在前 */
        for (k = 0; k < keep; k++)
            buf[k] = tmp[keep - 1 - k];

        return keep;
    }
}

/* ===================== 时间轴 + 汇总 ===================== */

/* 一天 1440 分钟，每位的"这分钟有事件"。
 * 180 字节，放栈上。 */
#define SL_MIN_BYTES    ((1440 + 7) / 8)

static void sl_min_set(rt_uint8_t *m, int minute)
{
    if (minute < 0 || minute >= 1440)
        return;
    m[minute >> 3] |= (rt_uint8_t)(1u << (minute & 7));
}

static rt_bool_t sl_min_test(const rt_uint8_t *m, int minute)
{
    if (minute < 0 || minute >= 1440)
        return RT_FALSE;
    return (m[minute >> 3] & (1u << (minute & 7))) ? RT_TRUE : RT_FALSE;
}

/* 扫一遍当天的 CSV，同时填时间轴与汇总。
 *
 * 为什么先用"分钟位图"再压缩，而不是直接往 70 档里塞：
 *   70 档的范围是 [第一次事件, 最后一次事件]，而这两个端点要扫完才知道。
 *   直接算桶号就得扫两遍文件（SD 上翻倍）。所以先按"当天第几分钟"这个
 *   绝对坐标记账（1440 位 = 180 字节），扫完拿到 first/last 再一次性
 *   压缩到 70 档。仍是单遍 I/O。
 *
 * 两个大数组用 static 而不是栈：分钟位图 180 字节 + 小时直方图 96 字节，
 * 加上本函数下面的 FatFs fopen/fgets 调用链，把 2KB 的描述符线程栈撑爆
 * 过（真机 "thread:snore_su stack overflow"）。static 安全——本函数只在
 * snore_sum 那一条线程里被调用，且与 UI 无并发。 */
int snore_log_day_scan(const char *date, snore_log_day_scan_t *out)
{
    char path[64];
    char line[SL_LINE_MAX];
    static rt_uint8_t   min_mask[SL_MIN_BYTES];
    static rt_uint32_t  hour_cnt[24];
    FILE *fp;
    rt_uint32_t prob_sum = 0;
    rt_uint32_t n_total = 0;
    rt_uint32_t first_sod = 0, last_sod = 0;
    rt_bool_t have_ts = RT_FALSE;
    int i;

    if (date == RT_NULL || out == RT_NULL)
        return -1;

    rt_memset(out, 0, sizeof(*out));
    rt_memset(min_mask, 0, sizeof(min_mask));
    rt_memset(hour_cnt, 0, sizeof(hour_cnt));

    rt_strncpy(out->sum.date, date, sizeof(out->sum.date) - 1);
    out->sum.date[sizeof(out->sum.date) - 1] = '\0';

    sl_path_of(date, path, sizeof(path));
    fp = fopen(path, "r");
    if (fp == RT_NULL)
        return 0;       /* 没有这天的文件：各项保持 0，不算错误 */

    while (fgets(line, sizeof(line), fp) != RT_NULL)
    {
        long ts;
        unsigned prob, cost;
        int sod;

        if (line[0] == '#')
            continue;
        if (strchr(line, ',') == RT_NULL)
            continue;
        /* 与 read_day / day_tail 同口径：sscanf 失败（掉电半行）跳过。
         * 三处口径必须一致，否则页面上的"总数"和明细条数对不上。 */
        if (sscanf(line, "%ld,%u,%u", &ts, &prob, &cost) != 3)
            continue;
        if (prob > 100)
            prob = 100;

        n_total++;
        prob_sum += prob;
        if (prob > out->sum.max_prob)
            out->sum.max_prob = prob;
        if (prob >= 90)
            out->sum.heavy++;

        /* 时间轴与时段分布只统计有日历时间的记录。
         * unknown.csv 的"时间戳"其实是开机秒数，用它算凌晨几点的分布
         * 会得出毫无意义的结论。 */
        if (ts > 0)
        {
            sod = (int)(ts % 86400);

            if (sod < 0 || sod >= 86400)
                continue;

            if (!have_ts)
            {
                first_sod = (rt_uint32_t)sod;
                last_sod  = (rt_uint32_t)sod;
                have_ts   = RT_TRUE;
            }
            else
            {
                if ((rt_uint32_t)sod < first_sod)
                    first_sod = (rt_uint32_t)sod;
                if ((rt_uint32_t)sod > last_sod)
                    last_sod = (rt_uint32_t)sod;
            }

            sl_min_set(min_mask, sod / 60);
            hour_cnt[sod / 3600]++;

            out->sum.last_ts   = (rt_uint32_t)ts;
            out->sum.last_prob = (rt_uint8_t)prob;
        }
    }
    fclose(fp);

    out->sum.total = n_total;
    out->axis.total = n_total;

    if (n_total > 0)
        out->sum.avg_prob = prob_sum / n_total;

    /* 峰值时段：相等时保留较早的那个（更符合"第一次集中出现"的直觉） */
    for (i = 0; i < 24; i++)
    {
        if (hour_cnt[i] > out->sum.peak_count)
        {
            out->sum.peak_count = hour_cnt[i];
            out->sum.peak_hour  = (rt_uint8_t)i;
        }
    }

    if (!have_ts)
        return 0;       /* 没有一条带日历时间的记录：时间轴留空 */

    /* 只有一次带日历时间的事件：first == last。
     *
     * 这里刻意**不**把 last_sod 撑成 first+60——UI 侧对 first>=last 有
     * 专门的"单事件"分支（画一条居中的短线）。撑开区间会让填桶逻辑把
     * 那 60 秒全当成有事件，画出一条横贯全轴的实心色块，与"只打了一次"
     * 的事实完全相反。
     *
     * 若还有其他无时间戳的记录，axis.total 会大于 1，但它们在时间轴上
     * 没有位置，UI 只看 first/last 与 total。 */
    if (last_sod == first_sod)
    {
        out->axis.first_sod = first_sod;
        out->axis.last_sod  = first_sod;
        return 0;
    }

    out->axis.first_sod = first_sod;
    out->axis.last_sod  = last_sod;

    /* 压缩到 70 档。每档覆盖的秒区间按比例划分，档内只要有一分钟
     * 有事件就置位——这样稀疏的孤立事件不会因为落在档边界而丢失。 */
    {
        rt_uint32_t span = last_sod - first_sod + 1;
        int b;

        for (b = 0; b < SNORE_LOG_AXIS_BUCKETS; b++)
        {
            rt_uint32_t s = first_sod + span * (rt_uint32_t)b / SNORE_LOG_AXIS_BUCKETS;
            rt_uint32_t e = first_sod + span * (rt_uint32_t)(b + 1) / SNORE_LOG_AXIS_BUCKETS;
            int m0, m1, m;

            if (e <= s)
                e = s + 1;

            m0 = (int)(s / 60);
            m1 = (int)((e - 1) / 60);

            for (m = m0; m <= m1; m++)
            {
                if (sl_min_test(min_mask, m))
                {
                    out->axis.mask[b >> 3] |= (rt_uint8_t)(1u << (b & 7));
                    break;
                }
            }
        }
    }

    return 0;
}

/* ===================== msh 调试命令 ===================== */

static int snore_log_cmd(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "days";

    if (rt_strcmp(cmd, "days") == 0)
    {
        snore_log_day_t days[7];
        int n = snore_log_list_days(days, 7);
        int i;

        if (n <= 0)
        {
            rt_kprintf("[snore_log] no archive (RTC not set or SD unavailable)\n");
            return 0;
        }
        for (i = 0; i < n; i++)
            rt_kprintf("  %s  %u events\n", days[i].date,
                       (unsigned)days[i].events);
        rt_kprintf("[snore_log] today=%u dropped=%u\n",
                   (unsigned)snore_log_today_count(),
                   (unsigned)snore_log_dropped());
        return 0;
    }

    if (rt_strcmp(cmd, "read") == 0 && argc > 2)
    {
        snore_log_rec_t recs[16];
        int n = snore_log_read_day(argv[2], recs, 16);
        int i;

        if (n <= 0)
        {
            rt_kprintf("[snore_log] no records for %s\n", argv[2]);
            return 0;
        }
        for (i = 0; i < n; i++)
        {
            rt_int64_t ts = recs[i].local_ts;
            int sod = (ts > 0) ? (int)(ts % 86400) : 0;

            rt_kprintf("  %02d:%02d:%02d prob=%u cost=%ums\n",
                       sod / 3600, (sod % 3600) / 60, sod % 60,
                       (unsigned)recs[i].prob, (unsigned)recs[i].cost_ms);
        }
        return 0;
    }

    if (rt_strcmp(cmd, "tail") == 0 && argc > 2)
    {
        snore_log_rec_t recs[8];
        rt_uint32_t total = 0;
        int n = snore_log_day_tail(argv[2], &total, recs, 8);
        int i;

        rt_kprintf("[snore_log] %s total=%u (showing newest %d)\n",
                   argv[2], (unsigned)total, n);

        for (i = 0; i < n; i++)
        {
            rt_int64_t ts = recs[i].local_ts;
            int sod = (ts > 0) ? (int)(ts % 86400) : 0;

            rt_kprintf("  %02d:%02d:%02d prob=%u cost=%ums\n",
                       sod / 3600, (sod % 3600) / 60, sod % 60,
                       (unsigned)recs[i].prob, (unsigned)recs[i].cost_ms);
        }
        return 0;
    }

    if (rt_strcmp(cmd, "now") == 0)
    {
        /* 时间状态一目了然：这个功能的生死线就是 time_valid。 */
        rt_kprintf("[snore_log] time_valid=%s time_ts=%ld\n",
                   g_app.time_valid ? "yes" : "NO",
                   (long)g_app.time_ts);
        return 0;
    }

    rt_kprintf("usage: snore_log days | read YYYY-MM-DD | "
               "tail YYYY-MM-DD | now\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(snore_log_cmd, snore_log, snore event archive on SD);
