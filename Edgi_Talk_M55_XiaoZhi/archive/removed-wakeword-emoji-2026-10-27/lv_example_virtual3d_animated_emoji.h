/*
 * port/lv_example_virtual3d_animated_emoji.h - 3D 表情符号桩头
 *
 * xiaozhi.h:13 无条件 include 官方 3D 表情头（原文件在 ui/3d_demo，依赖
 * lx_vglite GPU 库与 3.94MB TTF）。本工程按需求完全去除表情，不拷 3d_demo，
 * 此桩头只保留 xiaozhi.h/iot 所需的两个符号声明；qday_show_emoji_by_rtt_info
 * 的空实现在 xiaozhi_port.c，lv_example_virtual3d_animated_emoji 无存活调用点。
 */
#ifndef __LV_EXAMPLE_VIRTUAL3D_EMOJI_H__
#define __LV_EXAMPLE_VIRTUAL3D_EMOJI_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *lv_example_virtual3d_animated_emoji(lv_obj_t *parent);
void qday_show_emoji_by_rtt_info(int index);

#ifdef __cplusplus
}
#endif

#endif /* __LV_EXAMPLE_VIRTUAL3D_EMOJI_H__ */
