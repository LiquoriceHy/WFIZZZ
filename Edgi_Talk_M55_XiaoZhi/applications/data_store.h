/*
 * data_store.h - 用户数据的持久化落点（便签 + 闹钟共用）
 *
 * 与 app_cfg 的分工：
 *   app_cfg    = 凭据与地址，低频写，每次写入都重写整个 cfg.json。
 *   data_store = 用户数据（便签、闹钟），可能被 AI 高频写入。
 *
 * 分开为两个文件的原因：
 *   1) MCP 工具回调运行在 websocket 接收线程中（mcp_server.cpp:463），
 *      同步落盘会阻塞音频上行。因此 data_store 的所有写操作只修改内存，
 *      由调用方置脏标志，再由 app_data 的 1 秒线程统一落盘。
 *   2) 便签每次改动都重写 cfg.json 会使凭据文件被反复擦写，
 *      对卡寿命与故障面均不划算。
 *
 * 文件格式（扁平 JSON，与 cfg.json 采用同一套极简风格）：
 *   {
 *     "note0": "buy milk", "note0_done": "0",
 *     "note1": "", ...
 *     "alarm0_h": "7", "alarm0_m": "30", "alarm0_days": "31", "alarm0_en": "1",
 *     ...
 *   }
 *
 * 数字也存为字符串的原因：复用 app_cfg 那套只处理扁平字符串的极简解析器，
 * 不为数十个整数另写一套数字解析。读回时自行 atoi。
 *
 * 落盘路径：/sdcard/wfizzz/data.json（先写 .tmp，再 remove 目标，再 rename）。
 */
#ifndef __DATA_STORE_H__
#define __DATA_STORE_H__

#include <rtthread.h>

#define DATA_STORE_FILE      "/sdcard/wfizzz/data.json"
#define DATA_STORE_FILE_TMP  "/sdcard/wfizzz/data.json.tmp"

/* 把内存中的便签与闹钟整体写盘。
 *
 * 仅在 app_data 的落盘线程中调用（见 data_store_poll）。返回 0 表示成功。
 * SD 不可用时返回 -RT_EIO，调用方应保留脏标志等待下次重试。
 */
int data_store_save(void);

/* 从盘上读回便签与闹钟。文件不存在或损坏时返回负数，内存保持原值。 */
int data_store_load(void);

/* 将脏标志交给落盘线程。note 与 alarm 的所有写操作都调用此函数，不直接落盘。 */
void data_store_mark_dirty(void);

/* 是否存在待落盘的改动。 */
rt_bool_t data_store_is_dirty(void);

/* 落盘线程的入口：由 app_data 的 1 秒循环调用。
 *
 * 内部带去抖：仅在最后一次改动后静默达到 DATA_STORE_QUIET_MS 时才真正
 * 写盘，避免 AI 连续添加三条便签即触发三次整文件重写。
 */
void data_store_poll(void);

#endif /* __DATA_STORE_H__ */
