#ifndef __UI_PAGES_H__
#define __UI_PAGES_H__

#include "lvgl.h"

/* 在当前屏搭建页面框架：
 * 固定状态栏 + 横向滑动两页（主页/系统页）+ 页码点 + 30s 自动回主页。
 * 语音页已随语音链路退役（见 ui_pages.c 头注释）。 */
void ui_pages_init(void);

#endif
