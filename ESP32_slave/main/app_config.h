#pragma once

/* ============ 需要改的东西都集中在这里 ============
 * WiFi / broker / 设备 id / 引脚 / 心跳周期。
 * SSID/密码必须与 M55 侧连接的同一个 2.4G 网络一致；密码缺位会以
 * reason=15 握手超时 的形式表现，极像协议兼容性问题（2026-10-25 实踩）。
 *
 * 以下 WiFi 与 MQTT 凭据默认留空，请填写后编译。
 */

#define WIFI_SSID       ""                     /* 必须与 M55 同一个 2.4G 网络 */
#define WIFI_PASS       ""                     /* 至少 8 位，否则握手失败 */

#define MQTT_URI        ""                     /* 例如 "mqtt://192.168.1.100:1883" */
#define MQTT_USER       ""
#define MQTT_PASS       ""

#define DEV_ID          "esp32c6"               /* M55 设备表里的 id */
#define TOPIC_CMD       "wfizzz/dev/" DEV_ID "/cmd"
#define TOPIC_REPLY     "wfizzz/dev/" DEV_ID "/reply"
#define TOPIC_STATUS    "wfizzz/dev/" DEV_ID "/status"

#define LED_GPIO        8                       /* 板载 WS2812（RGB.h 同脚） */
#define EXT_LED_GPIO    3                       /* 外接 LED：高电平点亮 */
#define HEARTBEAT_MS    30000                   /* 协议要求 30-60s 心跳 */
