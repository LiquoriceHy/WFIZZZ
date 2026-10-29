#ifndef __UI_STATUSBAR_H__
#define __UI_STATUSBAR_H__

#include "lvgl.h"

/* 在 parent 顶部建 800x36 状态栏（WiFi/RSSI + MQTT 占位 + 电池 | IP），并自建 1s 刷新 */
lv_obj_t *ui_statusbar_create(lv_obj_t *parent);

/* 手动刷新一次（也由内部 lv_timer 每秒调用） */
void ui_statusbar_refresh(void);

#endif
