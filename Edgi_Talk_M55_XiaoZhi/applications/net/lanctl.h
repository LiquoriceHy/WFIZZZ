/*
 * lanctl.h - LAN 设备控制（MQTT）应用逻辑层
 *
 * 与 xz_app / snore_rt 同构：paho 独立客户端实例 + 快照 getter + 幂等
 * start / 尽力 stop。UI（ui_lanctl.c）只读快照、经 lanctl_dev_send 下发，
 * 不碰客户端内部。
 *
 * 与 snore 客户端的关系：同一个树莓派 broker 上的两个独立客户端。
 * 状态栏 MQTT 指示（app_data_set_mqtt）归 snore 所有，本模块不碰。
 */
#ifndef LANCTL_H
#define LANCTL_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 表容量上限（表本体在 lanctl_cfg.h，只被 lanctl.c 包含） */
#define LANCTL_DEV_MAX   4
#define LANCTL_CMD_MAX   6

/* 设备运行态快照（UI 每次刷新整体拷一份） */
typedef struct
{
    char        id[16];
    char        name[24];
    rt_bool_t   seen;          /* 收到过该设备的 status/reply */
    rt_bool_t   online;        /* status.online 为真且心跳未超时 */
    rt_uint32_t age_s;         /* 距最后一次报文的秒数（seen 为假时无意义） */
    char        ip[16];        /* status 上报的设备 IP */
    char        last_cmd[48];  /* 最近一次下发的 JSON（本地记录） */
    char        last_reply[96];/* 设备最近一条 reply 原文 */
} lanctl_dev_snapshot_t;

/* app_entry_t 四件套 */
int       lanctl_start(void);
void      lanctl_stop(void);
rt_bool_t lanctl_is_running(void);
void      lanctl_get_status(char *buf, int len);

/* MQTT 连接态（未编译 paho 时恒 RT_FALSE） */
rt_bool_t lanctl_mqtt_online(void);

/* 设备表访问 */
int         lanctl_dev_count(void);
void        lanctl_dev_snapshot(int idx, lanctl_dev_snapshot_t *out);
int         lanctl_dev_cmd_count(int idx);
const char *lanctl_dev_cmd_label(int idx, int cmd);

/* 下发第 idx 台设备的第 cmd 条命令（发布到 wfizzz/dev/<id>/cmd）。
 * 返回 0 成功，负 errno 失败（越界 / MQTT 未连接）。 */
int lanctl_dev_send(int idx, int cmd);

/* ---------------- AI / MCP 接口 ----------------
 *
 * MCP 工具回调跑在 websocket 接收线程里，不能长时间阻塞，因此下发与
 * 等回包分成两步：先 send 拿到序号，再 wait_reply 有界等待。 */

/* 按 AI 命令名（lanctl_cfg.h 的 name 字段）下发。
 * 成功返回 0，并把本次之前的 reply 序号写进 *seq_out、把该命令的协议名
 * （reply 里回显的 cmd 值）写进 *proto_out，供 lanctl_wait_reply 使用。
 * 失败返回负 errno（-RT_ENOENT 名字不存在，-RT_ERROR MQTT 未连）。 */
int lanctl_send_name(int idx, const char *name, rt_uint32_t *seq_out,
                     const char **proto_out);

/* 命令名列表，形如 "ping,led_on,led_off,reboot"（写进 buf，永不 NULL）。 */
void lanctl_dev_cmd_names(int idx, char *buf, int len);

/* 按名字查设备下标；找不到返回 -1。dev 可为 RT_NULL 表示"唯一设备"。 */
int lanctl_find_dev(const char *dev);

/* 有界等待第 idx 台设备出现"属于 expect_cmd 且序号 > after_seq"的新
 * reply（即本次命令的结果）。expect_cmd 传 RT_NULL 表示不校验归属。
 * timeout_ms 建议 <= 1500，避免拖住 websocket 接收线程。
 * 返回 1 拿到回包（out 内是 reply 原文），0 超时，负 errno 失败。 */
int lanctl_wait_reply(int idx, const char *expect_cmd, rt_uint32_t after_seq,
                      int timeout_ms, char *out, int len);

/* 当前 reply 序号（下发前取一次，配合 lanctl_wait_reply 判新旧）。 */
rt_uint32_t lanctl_reply_seq(int idx);

#ifdef __cplusplus
}
#endif

#endif /* LANCTL_H */
