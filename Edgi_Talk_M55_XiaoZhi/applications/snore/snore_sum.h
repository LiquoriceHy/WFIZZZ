/*
 * snore_sum.h - 最近 3 天鼾声汇总的后台缓存
 *
 * 为什么需要这一层（而不是让 UI 直接调 snore_log_day_tail）：
 *   snore_draw 跑在 LVGL 线程（优先级 21，栈 24KB，1ms 刷新周期），
 *   每 500ms 调用一次。一次汇总要 fopen + 逐行 fgets 扫完整个 CSV，
 *   3 天 = 3 次全文件扫描，SD 上每次数十毫秒，合起来 100ms 以上的 UI
 *   冻结，而且每 500ms 重来一遍。
 *
 *   所以归档读取放在独立线程里（优先级 27，栈 2KB），UI 只从内存快照读。
 *   这和 data_store_boot_task、cjk_font 的做法一致，也是本工程里所有
 *   "UI 需要文件内容"场景的统一范式。
 *
 * 谁启动：INIT_APP_EXPORT 常驻，不依赖鼾声检测是否在跑。用户想看历史
 * 记录时不该被要求先点 START —— 归档是只读的，和检测无因果关系。
 * 线程每秒醒一次，只在需要时才碰文件。
 *
 * 刷新时机：
 *   - 时间从无效变有效（NTP 校时成功）—— 这一天是功能能否出现的关键
 *   - 距上次刷新超过 SNORE_SUM_REFRESH_MS —— 捕获正在增长中的当天文件
 *   - UI 主动请求（翻页）—— 只对当前天立即重读
 *
 * 线程安全：快照由互斥量保护。UI 侧 snore_sum_get 拷贝一份出来用，
 * 拿到的是拷贝，之后归档线程再改也不影响 UI 的读取过程。
 */
#ifndef SNORE_SUM_H
#define SNORE_SUM_H

#include <rtthread.h>
#include "snore_log.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNORE_SUM_DAYS   3      /* 今天 + 前 2 天 */

typedef struct
{
    rt_bool_t           time_valid;   /* 生成这份快照时时钟是否可信 */
    rt_bool_t           sd_ok;        /* 生成快照时 SD 是否挂载 */
    char                date[SNORE_SUM_DAYS][16];   /* 从新到旧 */
    snore_log_day_scan_t day[SNORE_SUM_DAYS];       /* 时间轴 + 汇总 */
} snore_sum_snapshot_t;

/* 启动汇总线程（幂等，由 INIT_APP_EXPORT 调用一次）。 */
void snore_sum_start(void);

/* 取一份快照拷贝。
 *
 * 返回值表示"归档视图是否可用"：需要 time_valid 且 sd_ok 同时为真。
 * 返回 RT_FALSE 时调用方应回退到"本次开机"视图，并可用
 * out->time_valid / out->sd_ok 区分该提示用户什么。
 *
 * **可以在 LVGL 线程调用**：只做一次结构体拷贝（约 250 字节）+ 一次
 * 互斥量操作，不碰文件系统。 */
rt_bool_t snore_sum_get(snore_sum_snapshot_t *out);

/* 请求立即重读。不阻塞调用方，下一次线程循环生效。
 * UI 翻页时调用，让新选中的那天尽快刷新。 */
void snore_sum_request_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* SNORE_SUM_H */
