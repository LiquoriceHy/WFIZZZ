#ifndef __UI_SYSTEM_H__
#define __UI_SYSTEM_H__

#include "lvgl.h"

/* 系统页：WiFi/IP/对时/MQTT/运行时长/内存/固件 信息行 */
lv_obj_t *ui_system_create(lv_obj_t *parent);

#endif
