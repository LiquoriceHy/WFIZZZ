/*
 * lanctl_cfg.h - LAN 设备控制的编译期配置（设备表 + 每设备命令表）
 *
 * 本头文件带数据（static 设备表），只允许 lanctl.c 包含一次；其他模块
 * 经 lanctl.h 的 getter 访问。加设备 / 改命令只改这里，UI 与逻辑不动。
 *
 * ── 与被控设备（ESP32-C6 等）的消息协议 ──────────────────────────
 * broker：见下方 LANCTL_MQTT_URI（树莓派 mosquitto，与鼾声上报共用）
 * topic 树（<id> 为设备表里的 id，如 esp32c6）：
 *   wfizzz/dev/<id>/cmd     M55 → 设备   命令 JSON，内容 = 命令表的 json 字段
 *   wfizzz/dev/<id>/reply   设备 → M55   执行结果 JSON，原文显示在界面；
 *                           建议 {"ok":true,"cmd":"ping","msg":"..."}
 *   wfizzz/dev/<id>/status  设备 → M55   心跳/在线状态，retain 发，
 *                           遗嘱(LWT)发 {"online":false}；建议 30-60s 一次：
 *                           {"online":true,"ip":"192.168.1.x","rssi":-55}
 * M55 侧订阅一条通配 filter：wfizzz/dev/#
 * 设备端要求：
 *   1. client id 必须与 M55 互异（建议 dev_<id>，两台 M55 客户端分别是
 *      wfizzz_snore_* / m55_lanctl_*）
 *   2. 只订阅自己的 wfizzz/dev/<id>/cmd（QoS1）
 *   3. 连上后先发一次 retained status，之后按周期心跳；掉线由 LWT 报 offline
 */
#ifndef LANCTL_CFG_H
#define LANCTL_CFG_H

#include "lanctl.h"     /* LANCTL_DEV_MAX / LANCTL_CMD_MAX 在这里定义 */

/* broker 连接参数（与 snore_rt 同一个 mosquitto）
 * 请填写自己的 MQTT broker 地址与凭据，例如 "tcp://192.168.1.100:1883"。
 * 留空则 LAN 设备控制不启动 MQTT，界面上的按钮不会生效。 */
#define LANCTL_MQTT_URI    ""
#define LANCTL_MQTT_USER   ""
#define LANCTL_MQTT_PASS   ""

/* topic 前缀与通配订阅 filter（协议口径，一般不改） */
#define LANCTL_TOPIC_PREFIX   "wfizzz/dev/"
#define LANCTL_TOPIC_FILTER   LANCTL_TOPIC_PREFIX "#"

typedef struct
{
    const char *label;   /* 按钮文案，ASCII */
    const char *name;    /* AI 面向命令名（MCP 工具参数 action），ASCII、
                          * 小写下划线、跨版本稳定；与 label 分开是因为
                          * label 属界面文案，改版时不应影响已被模型记住
                          * 的命令名 */
    const char *proto;   /* 协议层 cmd 值（设备 reply 的 "cmd" 字段会原样
                          * 回显它），用于核对"这条 reply 是不是本次命令
                          * 的"。多条命令可共用同一个 proto（如 led_on /
                          * led_off 都是 "led"）。 */
    const char *json;    /* 下发到 cmd topic 的 JSON 原文 */
} lanctl_cmd_cfg_t;

typedef struct
{
    const char *id;      /* topic 内设备 id，ASCII，如 "esp32c6" */
    const char *name;    /* 界面显示名，ASCII */
    const lanctl_cmd_cfg_t cmds[LANCTL_CMD_MAX];  /* {RT_NULL,RT_NULL} 结尾 */
} lanctl_dev_cfg_t;

/* 设备表：加设备在这里加一项（<= LANCTL_DEV_MAX 项，多余槽位 id 填
 * RT_NULL）。name 字段供 MCP 的 self.lanctl.send 使用，改它等于改 AI
 * 能看到的命令名，UI 按钮不受影响。 */
static const lanctl_dev_cfg_t s_lanctl_devs[LANCTL_DEV_MAX] =
{
    {
        .id   = "esp32c6",
        .name = "ESP32-C6",
        .cmds =
        {
            { "Ping",    "ping",    "ping",   "{\"cmd\":\"ping\"}" },
            { "LED On",  "led_on",  "led",    "{\"cmd\":\"led\",\"state\":1}" },
            { "LED Off", "led_off", "led",    "{\"cmd\":\"led\",\"state\":0}" },
            { "Reboot",  "reboot",  "reboot", "{\"cmd\":\"reboot\"}" },
            { RT_NULL, RT_NULL, RT_NULL, RT_NULL },
        },
    },
};

#endif /* LANCTL_CFG_H */
