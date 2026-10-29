/*
 * ui_wifi.h - 屏上 WiFi 配网界面（从系统页点 WiFi 行进入）
 *
 * 形态：全屏覆盖层（与 ui_app 同思路，一次创建、显隐复用），两态：
 *   列表态：扫描结果列表，每行显示 SSID / 信号 / 加密标记；
 *           顶部工具条有 rescan / 断开 / 关闭。
 *   输入态：点选某个 SSID 后弹出密码输入（LVGL 自带 keyboard +
 *           textarea）与 Connect 按钮。
 *
 * 不占用抽屉中的应用位：
 *   抽屉仅剩 3 个槽位，而配网属于"设置类"操作，语义上归属系统页。
 *   从系统页 WiFi 行进入更符合直觉，也不占用应用槽位。
 *
 * 线程约束：
 *   LVGL 线程（优先级 21）不得阻塞。扫描 2-6 秒、连接 3-10 秒的工作
 *   全部交给 web_cfg 的后台线程，此处仅按 500ms 轮询状态。
 */
#ifndef UI_WIFI_H
#define UI_WIFI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在屏幕层创建 WiFi 界面（含全屏遮罩）。创建后处于隐藏状态。
 * 必须在 ui_theme_init() 之后且 web_cfg_start() 之后调用。 */
void ui_wifi_create(lv_obj_t *scr);

/* 打开 WiFi 界面（自动触发一次扫描）。重复调用安全。 */
void ui_wifi_open(void);

/* 关闭界面，返回下层界面。 */
void ui_wifi_close(void);

/* 当前是否处于打开状态。 */
rt_bool_t ui_wifi_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_WIFI_H */
