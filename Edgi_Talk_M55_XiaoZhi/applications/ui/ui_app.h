/*
 * ui_app.h - 应用全屏屏幕（点开应用 = 进入一个新屏幕）
 *
 * 与抽屉的区别：抽屉是覆盖在主页之上的 340px 面板，用于选择应用；
 * 应用屏幕是 800x480 的全屏独立页面，带有自己的标题栏与返回按钮，
 * 会覆盖状态栏与 pager，语义上即"进入了一个 app"。
 *
 * 使用对象层而非 lv_screen_load()：屏幕对象复用同一块显示缓冲，
 * 整屏切换代价高且会打断 pager 的滚动状态。此处改为在活动屏幕上挂一个
 * 全屏不透明容器，覆盖一切，隐藏即返回。
 *
 * 一次创建、显隐复用（LVGL 走 RT-Thread 堆，避免反复分配）。
 */
#ifndef UI_APP_H
#define UI_APP_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在屏幕层创建应用屏骨架（含标题栏与返回按钮）。创建后处于隐藏态。
 * 必须在 ui_theme_init() 之后调用。 */
void ui_app_create(lv_obj_t *scr);

/* 打开第 index 个应用的屏幕。越界或被现有应用占用时忽略。
 * 屏幕内容由 app_registry 的 draw 回调决定（见 app_entry_t.draw）。 */
void ui_app_open(int index);

/* 关闭当前应用屏幕，回到下层界面（抽屉在进应用时已收起，实际回到
 * 主页仪表盘）。 */
void ui_app_close(void);

/* 当前是否停在某个应用屏幕里。 */
rt_bool_t ui_app_is_open(void);

/* 当前打开的应用下标；未打开时为 -1。 */
int ui_app_current(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_APP_H */
