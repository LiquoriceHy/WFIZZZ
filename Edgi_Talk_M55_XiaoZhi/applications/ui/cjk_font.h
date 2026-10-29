/*
 * cjk_font.h - 运行时加载的中文字库
 *
 * 用法（ui_theme.c 里已经接好）：
 *   1. 开机 UI 初始化之后调 cjk_font_init()
 *   2. 之后所有取字体的地方用 ui_font_text() / ui_font_small()，
 *      它们会在字库加载成功时返回运行时字库，否则返回编译期内置字库
 *
 * 这样即使 SD 卡没插、字库文件缺失，界面也不会因为字体为空而整片空白。
 */
#ifndef CJK_FONT_H
#define CJK_FONT_H

#include <rtthread.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 加载 SD 卡上的字库。返回 0 成功，-1 失败（调用方应回退到内置字库）。
 * 幂等：重复调用只真正执行一次。
 *
 * 注意这是同步版本，会阻塞最长约 5 秒（等 SD 挂载的 25 次重试）。
 * 界面初始化路径请用 cjk_font_start_async()。 */
int cjk_font_init(void);

/* 开线程异步加载，界面初始化时调这个（不阻塞 LVGL 主循环）。
 * 加载成功后线程自己调 ui_theme_apply_cjk_font() 换字体。 */
void cjk_font_start_async(void);

/* 运行时字库对象；未加载成功时返回 RT_NULL */
lv_font_t *cjk_font_get(void);

/* 是否已加载成功 */
rt_bool_t cjk_font_ready(void);

/* msh 命令 cjk：打印字库状态与 HyperRAM 用量 */
void cjk_font_info(void);

#ifdef __cplusplus
}
#endif

#endif /* CJK_FONT_H */
