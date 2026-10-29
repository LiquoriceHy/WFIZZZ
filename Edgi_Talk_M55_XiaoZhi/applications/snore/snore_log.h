/*
 * snore_log.h - 鼾声事件按天归档到 SD
 *
 * 文件布局（每天一个文件，追加写）：
 *   /sdcard/wfizzz/snore/2026-10-27.csv
 *
 * CSV 格式（选择 CSV 而非 JSON 的原因：追加写无需读取全文再回写，
 * 掉电最多丢失最后一行，不会像 JSON 那样留下无法解析的半截文件）：
 *   # date,start_ts
 *   1793088000,93,412
 *   ...
 * 列：local_ts(秒),prob(0-100),cost_ms
 *
 * 时间来源为 g_app.time_ts（RTC 加时区，NTP 校准后更准确）。
 * 若 RTC 未设置（time_valid=false），事件仍会记录，但写入 unknown.csv，
 * 并以开机秒数作为时间戳，宁可归档时间不准，也不丢失用户真实打鼾的记录。
 */
#ifndef SNORE_LOG_H
#define SNORE_LOG_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNORE_LOG_DIR        "/sdcard/wfizzz/snore"
#define SNORE_LOG_QUEUE_LEN  16

/* 一条待落盘事件 */
typedef struct
{
    rt_int64_t local_ts;    /* 本地时间戳；ts_valid 为假时无意义 */
    rt_uint32_t uptime_s;   /* 开机秒数，ts_valid 为假时用它 */
    rt_uint16_t prob;       /* 0-100 */
    rt_uint16_t cost_ms;
    /* 时间有效性在 append 时快照，不在归档线程里重新判断：
     * 归档线程可能晚几百毫秒，期间 NTP 若校时成功，time_valid 会翻成真，
     * 那样一条真实发生在未校时期间的事件会被错记到校时后的日期里。
     *
     * 判据必须是 time_valid 而非 local_ts > 0：软 RTC 从 2018-01-01 起算，
     * 未校时的 time_ts 也恒为正，只看符号会把事件写进 2018-01-0X.csv
     * 这种"看似合法但完全错误"的文件。 */
    rt_bool_t  ts_valid;
} snore_log_rec_t;

/* 启动归档线程（幂等）。由 snore_rt_start 调用，不单独 INIT_EXPORT，
 * 因为只有检测真正开跑才会产生事件。 */
void snore_log_start(void);

/* 停止归档线程。会把队列里剩下的记录写完再退出。 */
void snore_log_stop(void);

/* 投递一条事件。本函数非阻塞：队列满时丢弃并计数。
 *
 * 必须异步的原因：调用点位于推理线程（优先级 24），而一次 SD 追加写
 * 在 FatFs 上需数十毫秒（含目录项更新）。同步写会使 337ms 一窗的推理
 * 节奏变得不确定，实时余量（2.9 倍）将被显著消耗。 */
void snore_log_append(rt_uint16_t prob, rt_uint32_t cost_ms);

/* 汇总：最近 N 天（含今天）的每日条数。用于 AI 工具和 UI。
 * days 数组按日期从新到旧填充。
 * 返回实际填充的天数（最多 max），0 表示没有日志。
 *
 * 结构刻意保持扁平：调用方（MCP 工具回调）跑在 ws 接收线程，
 * 不能做重活，只读文件头几行即可。 */
typedef struct
{
    char        date[16];   /* "2026-10-27" 或 "unknown" */
    rt_uint32_t events;     /* 当天事件条数 */
} snore_log_day_t;

int snore_log_list_days(snore_log_day_t *buf, int max);

/* 读某一天的完整记录。返回条数。
 * ts 数组可为 NULL（只取计数）。
 *
 * **从文件头开始读**，max 小于总行数时只返回最早的 max 条。
 * 要"最新几条"请用 snore_log_day_tail()。 */
int snore_log_read_day(const char *date, snore_log_rec_t *buf, int max);

/* 一遍扫描算出当日总数，并取出**最新**的 max 条事件（放到 buf，最新在前）。
 *
 * 为什么不让调用方自己 read_day：
 *   1. read_day 从文件头读，只能拿到最早的 N 条，而卡片要显示最新的。
 *   2. 单日上限 5760 条（15 秒冷却），全读进内存要 92KB，而主堆余量
 *      只有约 40KB。本函数用环形缓冲，内存只与 max 成正比。
 *
 * total 可为 NULL（只取最新几条）。
 * 返回值：实际填入 buf 的条数（<= max）；文件不存在时返回 0 且 *total=0。
 * buf 为 NULL 时只算 total。 */
int snore_log_day_tail(const char *date, rt_uint32_t *total,
                       snore_log_rec_t *buf, int max);

/* ===================== 时间轴 + 汇总（UI 图表用） =====================
 *
 * 把一天的记录压成"横轴 = 时间"的占用位图，外加一组汇总统计。
 * 供 Snore 应用页左侧时间轴图表 + 右侧汇总卡使用。
 *
 * 为什么用位图而不是返回时间戳数组：
 *   单日上限 5760 条（15 秒冷却），时间戳数组要 23KB，而 UI 轨道只有
 *   约 350px 宽，横向精度本来就只有 5px 一档（70 个桶）。9 字节位图
 *   就把"哪个时段有事件"表达完整了，且密集事件自动连成连续色块，
 *   正是图表想要的视觉。
 *
 * 为什么 axis 与 sum 合并成一次扫描：
 *   两者都要遍历整个 CSV。分开调 = 同一个文件扫两遍，SD 上翻倍。
 *   后台线程每 60 秒扫 3 天，能省下 3 次全文件读。 */

#define SNORE_LOG_AXIS_BUCKETS  70    /* 350px 轨道 / 5px 一档 */
#define SNORE_LOG_AXIS_BYTES    ((SNORE_LOG_AXIS_BUCKETS + 7) / 8)   /* 9 */

typedef struct
{
    rt_uint32_t total;       /* 当日事件总数 */
    rt_uint32_t first_sod;   /* 第一次事件的当天秒数 0..86399 */
    rt_uint32_t last_sod;    /* 最后一次事件的当天秒数 */
    rt_uint8_t  mask[SNORE_LOG_AXIS_BYTES];   /* 每个时间档是否有事件 */
} snore_log_axis_t;

typedef struct
{
    char        date[16];   /* "2026-10-28" */
    rt_uint32_t total;      /* 当日事件总数 */
    rt_uint32_t avg_prob;   /* 平均概率 0-100 */
    rt_uint32_t max_prob;   /* 最高概率 0-100 */
    rt_uint32_t heavy;      /* prob >= 90 的条数 */
    rt_uint32_t last_ts;    /* 最后一条事件的时间戳（当天，0 表示没有） */
    rt_uint8_t  last_prob;  /* 最后一条事件的概率 */
    rt_uint8_t  peak_hour;  /* 事件最多时段的起始整点 0-23 */
    rt_uint32_t peak_count; /* 该时段的事件数 */
} snore_log_summary_t;

/* 时间轴 + 汇总的单次扫描结果 */
typedef struct
{
    snore_log_axis_t    axis;
    snore_log_summary_t sum;
} snore_log_day_scan_t;

/* 扫一遍当天的 CSV，同时填时间轴与汇总。
 *
 * 返回 0 成功（文件不存在时 total=0，其余留零，不算错误），-1 参数非法。 */
int snore_log_day_scan(const char *date, snore_log_day_scan_t *out);

/* 本地时间戳 → "YYYY-MM-DD"（ts<=0 时写 "unknown"）。
 *
 * 公开出来是给"按天翻页"用的：分页栏需要算出今天/昨天/前天的日期串，
 * 而这个 civil_from_days 算法在 snore_log.c 里已有一份，没必要再抄一遍
 * （抄错会导致 UI 显示的日期与实际文件名不一致，比缺这个接口糟糕得多）。
 *
 * 注意 ts 是**本地时间**（已加 AD_TZ_OFFSET_SEC），与 g_app.time_ts 同源。 */
void snore_log_date_of(rt_int64_t ts, char *out, rt_size_t len);

/* 今日事件数（内存中的快速计数，不读文件） */
rt_uint32_t snore_log_today_count(void);

/* 丢弃计数（队列满导致），调试用 */
rt_uint32_t snore_log_dropped(void);

#ifdef __cplusplus
}
#endif

#endif /* SNORE_LOG_H */
