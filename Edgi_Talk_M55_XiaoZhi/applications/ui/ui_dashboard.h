#ifndef __UI_DASHBOARD_H__
#define __UI_DASHBOARD_H__

#include "lvgl.h"

/* 主页：大时钟 + 日期 + TEMP/HUMIDITY/WEATHER 卡片 + 当月日历。
 * parent 尺寸由 ui_pages 提供固定 800x444，内部绝对定位。 */
lv_obj_t *ui_dashboard_create(lv_obj_t *parent);

#endif
