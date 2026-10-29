/*
 * lv_port.c - UI 入口：仪表盘（主页 + 系统页，横滑两页）
 *
 * 结构：ui_pages.c 搭建框架（状态栏、横滑容器、页码点、自动回主页），
 * 主页 ui_dashboard.c（大时钟/日期/温湿度/墨墨卡/闹钟与便签卡/日历），
 * 系统页 ui_system.c（IP/内存/CPU/电池/运行时长）。
 * 语音页已随语音链路退役，见 ui_pages.c 头注释。
 *
 * 小智聊天界面不再是独立全屏页：它作为抽屉中的 "AI Chat" 应用
 * （app_registry.c 的 xz_draw/xz_add_bubble）挂在 ui_app 的 app_body 内，
 * 以便复用统一的顶栏 Back/START。旧的 ui_chat.c 已删除，因为它被
 * --gc-sections 整段回收、从未被调用，保留只会拖慢编译。
 */
#include <rtthread.h>
#include "lvgl.h"
#include "ui_theme.h"
#include "ui_pages.h"
#include "screen_saver.h"
#include "cjk_font.h"

void lv_user_gui_init(void)
{
#if LV_USE_PERF_MONITOR
    /* LVGL 自带 CPU/FPS 悬浮框用 sysmon 公开 API 隐藏（lv_conf.h 关掉
     * LV_USE_PERF_MONITOR 后本段自动编译为空） */
    lv_sysmon_hide_performance(lv_display_get_default());
#endif

    ui_theme_init();
    ui_pages_init();

    /* 息屏定时器必须在 LVGL 起来之后创建（依赖 lv_timer 与显示设备） */
    screen_saver_init();

    /* 中文字库异步加载，不得阻塞此处。
     *
     * 本函数由 LVGL 主线程调用（lv_rt_thread_port.c:77），返回后才进入
     * lv_timer_handler() 循环。字库需读取 860KB，而 SD 挂载由另一条低
     * 优先级线程完成，首次读取很可能失败；若带重试则最长等待 5 秒，
     * 这 5 秒内界面完全无响应，用户会误认为死机。
     *
     * 因此另开一条独立线程加载。加载完成时该线程自行调用
     * ui_theme_apply_cjk_font() 切换字体并重绘，用户看到的效果是
     * "英文界面先出现，一两秒后中文补上"。 */
    cjk_font_start_async();
}
