/*
 * ui_lanctl.h - LAN Devices 全屏应用页
 *
 * 实现 ui_app 宿主契约的 draw/undraw，供 app_registry.c 的应用条目转发。
 */
#ifndef __UI_LANCTL_H__
#define __UI_LANCTL_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_lanctl_draw(lv_obj_t *parent);
void ui_lanctl_undraw(lv_obj_t *parent);

#ifdef __cplusplus
}
#endif

#endif /* __UI_LANCTL_H__ */
