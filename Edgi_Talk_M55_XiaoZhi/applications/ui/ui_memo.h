/*
 * ui_memo.h - 主页墨墨背单词卡片（今日进度 + 认识/模糊/忘记三色条）
 */
#ifndef __UI_MEMO_H__
#define __UI_MEMO_H__

#include "lvgl.h"

/* 在 parent 的 (x, y) 处创建 168x168 卡片（主页调用：底边与日历对齐） */
lv_obj_t *ui_memo_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y);

#endif
