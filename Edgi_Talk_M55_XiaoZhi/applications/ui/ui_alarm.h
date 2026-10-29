/*
 * ui_alarm.h - 闹钟 UI：主页卡片 + 二级设置界面 + 响铃遮罩
 */
#ifndef __UI_ALARM_H__
#define __UI_ALARM_H__

#include "lvgl.h"

/* 主页闹钟卡片（ui_dashboard 在页面坐标系里创建，点卡片打开二级界面） */
lv_obj_t *ui_alarm_card_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y);

/* 响铃遮罩（ui_pages 挂到屏幕层；内部 200ms 轮询 g_app.alarm_ring，
 * 触发时全屏闪烁置顶显示，点任意处停止，3 分钟无人理睬由 alarm 线程超时） */
void ui_alarm_ring_create(lv_obj_t *parent);

/* 打开闹钟二级设置界面（全屏覆盖层，列表 + 编辑两态） */
void ui_alarm_open(void);

#endif
