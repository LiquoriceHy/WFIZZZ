#pragma once

#include <stdbool.h>

/* WiFi STA：开机一次性 bring-up，之后断线自动重连。
 * 状态全部走只读接口，供状态屏/状态发布轮询。 */

void wifi_sta_start(void);          /* 内含 netif/event loop/wifi 全套初始化 */

bool wifi_sta_is_up(void);          /* 已拿到 IP */
int  wifi_sta_last_reason(void);    /* 最近一次断连原因码（屏上辅助定位） */
bool wifi_sta_api_ready(void);      /* esp_wifi_start 完成后才允许调任何 WiFi API：
                                     * 提前调用会死在内部未创建的互斥锁上（实测
                                     * 拖死 lvgl 任务+看门狗） */
void wifi_sta_get_ip(char *buf, int len);   /* 未联网时填 "-" */
int  wifi_sta_get_rssi(void);               /* 未就绪/未连上返回 0 */
