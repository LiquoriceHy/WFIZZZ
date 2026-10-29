/*
 * ui_app_icons.h - 抽屉卡片图标的绘制入口
 *
 * 图标全部用 LVGL 图元（矩形 + 折线）现画，不依赖位图资源。
 * app_registry 只声明要哪个图形（app_icon_t），由 ui_app_icon_draw()
 * 一处分发，所以新增应用不需要碰 ui_drawer.c。
 */
#ifndef __UI_APP_ICONS_H__
#define __UI_APP_ICONS_H__

#include "lvgl.h"
#include "app_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在 icon 容器（40x40，背景由调用方设为墨黑）中绘制指定图标。
 * color 为前景色（24 位 0xRRGGBB），抽屉中传入卡片白。
 *
 * APP_ICON_LETTER 为空操作，该分支由调用方自行绘制首字母。 */
void ui_app_icon_draw(lv_obj_t *icon, app_icon_t id, uint32_t color);

/* 复位内部的线段点集池。抽屉创建完成后即可调用；目前仅在
 * ui_drawer_create() 中调用一次，防止极端情况下重复建卡耗尽池。 */
void ui_app_icon_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* __UI_APP_ICONS_H__ */
