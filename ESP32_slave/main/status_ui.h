#pragma once

/* ST7789 240x240 状态屏（LVGL 8，驱动来自 ESP32C6_Learn 板级工程）。
 * 全部 LVGL 调用收在内部单任务里：首次迭代建界面，之后每 500ms 刷新，
 * 屏显 WiFi/MQTT/IP/RSSI/uptime/LED/最近命令(>)/回包(<)。 */

void status_ui_start(void);                /* LCD+LVGL 初始化并起刷新任务 */

/* 跨任务共享字符串（内部临界区保护），cmd/reply 层写入、任务内读取 */
void status_ui_set_cmd(const char *txt);   /* 最近一次收到的命令 */
void status_ui_set_reply(const char *txt); /* 最近一条回包（截断显示） */
