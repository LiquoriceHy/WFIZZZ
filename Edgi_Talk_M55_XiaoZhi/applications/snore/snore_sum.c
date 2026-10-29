/*
 * snore_sum.c - 最近 3 天鼾声汇总的后台缓存
 *
 * 职责：在独立线程里定期读归档 CSV，把结果放进受互斥量保护的快照，
 * 让 LVGL 线程（snore_draw，每 500ms）只做内存拷贝就能拿到数据。
 *
 * 线程参数取舍：
 *   栈 4KB —— 调用链 snore_log_day_scan() 内部有 FatFs 的 fopen/fgets
 *             调用链（newlib-nano 的 stdio 与 elmfat 的 FIL 结构都不小）。
 *             曾经取 2KB 并把大数组放栈上，真机直接报
 *             "thread:snore_su stack overflow"。现在 180 字节的分钟位图、
 *             96 字节的小时直方图、以及 3 份快照都已改为 static，
 *             4KB 是给 FAT/stdio 调用链留的余量。
 *   prio 27 —— 低于 app_data(24)、UI(21)、音频(20)。读盘慢一点无所谓，
 *             绝不能让归档扫描抢占 UI 重绘。
 *
 * 为什么不复用 snore_log 的归档线程：那条线程只处理写队列，且与检测
 * 线程同生命周期（不检测就没有写，线程不启动）。但"看历史记录"不该
 * 要求用户先开启检测，所以汇总必须独立于检测常驻。
 */

#include <rtthread.h>
#include <sys/stat.h>

#include "snore_sum.h"
#include "app_data.h"

#define DBG_TAG "snore.sum"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define SNORE_SUM_STACK       4096
#define SNORE_SUM_PRIO        27
#define SNORE_SUM_TICK        20

/* 周期重读间隔。当天文件在检测运行时持续增长，60 秒足够让"今日总数"
 * 看起来是活的；同时把 SD 读次数压到每小时 60 次这个量级。
 * 不要缩短到 500ms（draw 的周期）：那等于每 500ms 扫一遍全文件。 */
#define SNORE_SUM_REFRESH_MS  60000

/* 等 SD 挂载的重试间隔（开机时 mnt.c 的 hotplug 线程要 200ms 起步） */
#define SNORE_SUM_SD_WAIT_MS  2000

static snore_sum_snapshot_t s_snap;
static struct rt_mutex       s_lock;
static rt_bool_t             s_lock_ready = RT_FALSE;

static volatile rt_bool_t s_run       = RT_FALSE;
static volatile rt_bool_t s_refresh   = RT_FALSE;
static rt_thread_t        s_thread    = RT_NULL;

/* ===================== 读盘 ===================== */

static rt_bool_t sum_sd_ready(void)
{
    struct stat st;

    return (stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode)) ? RT_TRUE : RT_FALSE;
}

/* 提交快照。只在汇总线程里写，锁是为了挡住 UI 侧的并发读。 */
static void sum_publish(const snore_sum_snapshot_t *tmp)
{
    if (s_lock_ready)
        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);

    s_snap = *tmp;

    if (s_lock_ready)
        rt_mutex_release(&s_lock);
}

/* 重读 3 天。调用方保证在汇总线程里。
 *
 * tmp 用 static 而不是栈上局部量：它含 3 份 snore_log_day_scan_t
 * （每份约 84 字节）。放栈上加上下游 snore_log_day_scan 内部的分钟位图
 * （180 字节）、小时直方图（96 字节）与 FatFs 的 FILE 结构，实测会把
 * 2KB 栈撑爆（真机报 "thread:snore_su stack overflow"）。
 *
 * static 安全：本函数只被 snore_sum 线程调用，且是单实例。 */
static void sum_read_all(void)
{
    static snore_sum_snapshot_t tmp;
    rt_int64_t ts;
    int i;

    rt_memset(&tmp, 0, sizeof(tmp));

    if (!g_app.time_valid)
    {
        /* 时钟不可信时按天枚举没有意义：软 RTC 从 2018-01-01 起算，
         * 算出来的"今天"是个假日期，读到的文件也不是用户的今天。
         * 沿用上一份快照的日期串，只把 time_valid 置假让 UI 切视图。
         * sd_ok 必须重新探测：用户可能刚插卡，用旧值会让提示停在
         * "no SD card"，即使卡已经可用。 */
        if (s_lock_ready)
            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        tmp = s_snap;
        if (s_lock_ready)
            rt_mutex_release(&s_lock);

        tmp.time_valid = RT_FALSE;
        tmp.sd_ok      = sum_sd_ready();
        sum_publish(&tmp);
        return;
    }

    tmp.time_valid = RT_TRUE;
    tmp.sd_ok      = sum_sd_ready();
    ts = g_app.time_ts;

    for (i = 0; i < SNORE_SUM_DAYS; i++)
    {
        /* 与 snore_log_list_days 相同的"取当天正午"技巧：若 ts 恰好落在
         * 午夜附近，直接用 ts-i*86400 可能把相邻两天算成同一天。 */
        rt_int64_t day_ts = ts - (rt_int64_t)i * 86400;

        snore_log_date_of(day_ts - (day_ts % 86400) + 12 * 3600,
                          tmp.date[i], sizeof(tmp.date[i]));

        if (tmp.sd_ok)
        {
            /* 一次扫描同时拿到时间轴与汇总：分开调会把同一个文件扫两遍，
             * 而 3 天 × 2 = 6 次全文件读对 SD 和本线程都不划算。 */
            (void)snore_log_day_scan(tmp.date[i], &tmp.day[i]);
        }
        else
        {
            rt_memset(&tmp.day[i], 0, sizeof(tmp.day[i]));
        }
    }

    sum_publish(&tmp);
}

/* ===================== 线程 ===================== */

static void snore_sum_entry(void *param)
{
    rt_bool_t last_time_valid = RT_FALSE;
    rt_uint32_t last_ms = 0;

    RT_UNUSED(param);

    while (s_run)
    {
        rt_uint32_t now = rt_tick_get_millisecond();
        rt_bool_t need = RT_FALSE;

        /* 触发条件一：时间刚变可信。开机时 NTP 还没成功，快照里是空的；
         * 校时成功后必须立刻重读一次，否则用户要等满一个刷新周期才看到
         * 归档（表现为"时间对了但记录还是空的"）。 */
        if (g_app.time_valid && !last_time_valid)
            need = RT_TRUE;

        /* 触发条件二：周期刷新，捕获正在增长的当天文件 */
        if (last_ms == 0 ||
            (rt_uint32_t)(now - last_ms) >= SNORE_SUM_REFRESH_MS)
            need = RT_TRUE;

        /* 触发条件三：UI 主动请求（翻页） */
        if (s_refresh)
        {
            s_refresh = RT_FALSE;
            need = RT_TRUE;
        }

        if (need)
        {
            sum_read_all();
            last_ms = rt_tick_get_millisecond();
            last_time_valid = g_app.time_valid;
        }

        /* 100ms 一跳：UI 翻页要能快速响应，而空转的代价只是唤醒一次线程。
         * 真正的读盘由上面三个条件把关，不会每次唤醒都碰文件。 */
        rt_thread_mdelay(100);
    }

    s_thread = RT_NULL;
}

/* ===================== 对外接口 ===================== */

void snore_sum_start(void)
{
    if (s_thread != RT_NULL)
        return;

    if (!s_lock_ready)
    {
        if (rt_mutex_init(&s_lock, "ssum", RT_IPC_FLAG_FIFO) == RT_EOK)
            s_lock_ready = RT_TRUE;
    }

    s_run = RT_TRUE;

    s_thread = rt_thread_create("snore_sum", snore_sum_entry, RT_NULL,
                                SNORE_SUM_STACK, SNORE_SUM_PRIO, SNORE_SUM_TICK);
    if (s_thread == RT_NULL)
    {
        LOG_E("create thread failed");
        s_run = RT_FALSE;
        return;
    }
    rt_thread_startup(s_thread);
    LOG_I("summary thread started");
}

rt_bool_t snore_sum_get(snore_sum_snapshot_t *out)
{
    if (out == RT_NULL)
        return RT_FALSE;

    if (s_lock_ready)
        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);

    *out = s_snap;

    if (s_lock_ready)
        rt_mutex_release(&s_lock);

    /* 归档视图可用的条件：时钟可信 + SD 可用。
     * 缺任一条，UI 都要回退到"本次开机"视图。 */
    return (out->time_valid && out->sd_ok) ? RT_TRUE : RT_FALSE;
}

void snore_sum_request_refresh(void)
{
    s_refresh = RT_TRUE;
}

static int snore_sum_auto_start(void)
{
    snore_sum_start();
    return RT_EOK;
}
INIT_APP_EXPORT(snore_sum_auto_start);
