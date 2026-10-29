#pragma once

#include <stdbool.h>

/* MQTT 链路：LAN Devices 协议的传输层（broker/账号/topic 见 app_config.h）。
 * 收到 cmd 转给 dev_cmd 处理；publish_reply/publish_status 供命令层与心跳用。 */

void mqtt_link_start(void);                      /* 内含 30s 心跳任务 */
bool mqtt_link_is_up(void);
void mqtt_link_publish_reply(const char *json);  /* 掉线时告警并丢弃 */
void mqtt_link_publish_status(void);             /* retained；M55 一订阅即见 */
