/*
 * ui_drawer.h - 底部上滑应用抽屉
 *
 * 交互：在屏幕底部 36px 热区向上滑（位移 > 50px）拉出抽屉；
 *       点遮罩空白处或点 Close 按钮收起。
 * 内容：由 app_registry 提供的应用卡片列表（图标块 + 名称 + 实时状态 + 按钮）。
 *
 * 实现要点（与工程既有风格一致）：
 *   - 一次创建、显隐复用（LVGL 堆仅 64KB，避免反复 malloc）
 *   - 全屏遮罩 + 底部面板，面板用 lv_anim 做 y 位移滑入/滑出
 *   - LVGL 9.2 没有 lv_obj_animate，必须用 lv_anim_t + 自定义 exec
 *   - 抽屉可见期间用定时器触发 activity，防止 60s 息屏把屏幕关掉
 */
#ifndef UI_DRAWER_H
#define UI_DRAWER_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在屏幕层创建抽屉（含遮罩）。必须在 ui_theme_init() 之后调用。
 * 创建后处于隐藏态，由 ui_drawer_open() 拉出。 */
void ui_drawer_create(lv_obj_t *scr);

/* 拉出抽屉（带动画）。重复调用安全。 */
void ui_drawer_open(void);

/* 收起抽屉（带动画）。 */
void ui_drawer_close(void);

rt_bool_t ui_drawer_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_DRAWER_H */
