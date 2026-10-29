/*
 * app_registry.h - 应用注册表：抽屉里"有哪些应用"的唯一真相来源
 *
 * 设计意图：UI 只认识 app_entry_t，不认识任何具体模块。以后新增应用
 * 只需在 app_registry.c 的数组里加一项，ui_drawer.c 一行都不用改。
 *
 * 字段全为可选：不可启动的应用把 start/stop 置 NULL，UI 就不画按钮；
 * get_status 为 NULL 时卡片只显示 Running/Idle。
 */
#ifndef APP_REGISTRY_H
#define APP_REGISTRY_H

#include <rtthread.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 抽屉卡片的图标标识。
 *
 * 用枚举而不是函数指针：图标的实现集中在 ui_app_icons.c，注册表只声明
 * "要哪个图形"，UI 侧一处 switch 分发。这样 app_registry.c 不需要包含
 * 任何 LVGL 绘制细节，新增应用仍然只改数组。 */
typedef enum
{
    APP_ICON_LETTER = 0,   /* 兜底：画名字首字母（旧行为） */
    APP_ICON_SNORE,        /* Z + 声波弧 */
    APP_ICON_AUDIO,        /* 电平竖条 */
    APP_ICON_CHAT,         /* 对话气泡 */
    APP_ICON_MODEL,        /* NPU 芯片 */
    APP_ICON_LAN,          /* 网络节点 */
    APP_ICON_NUM,
} app_icon_t;

typedef struct app_entry
{
    const char *name;                        /* 卡片标题，ASCII */
    const char *desc;                        /* 副标题/用途说明，ASCII */
    app_icon_t  icon;                        /* 抽屉卡片图标 */
    rt_bool_t   uses_mic;                    /* 占用 mic0：启动前需互斥 */

    /* 运行状态查询；NULL 视为"不可枚举运行态" */
    rt_bool_t (*is_running)(void);

    /* 启停；start 为 NULL 表示只读应用（不画按钮） */
    int  (*start)(void);
    void (*stop)(void);

    /* 一行状态文案（长度建议 <40 字符）；NULL 时用 is_running 生成 */
    void (*get_status)(char *buf, int len);

    /* 详情页的多行文本（换行用 '\n'，建议 <=6 行）；NULL 则不显示 */
    void (*get_detail)(char *buf, int len);

    /* 全屏应用屏幕。draw 在 parent（内容区容器）中创建控件，打开时调用一次，
     * 之后每 500ms 调用一次用于刷新文本，因此实现必须保证"重复调用安全"，
     * 即已创建的控件只更新内容、不重复创建。
     * undraw 在离开屏幕前调用，用于释放本应用创建的控件（parent 本身保留）。
     * 两者均为 NULL 时，屏幕只显示标题与启停按钮。 */
    void (*draw)(lv_obj_t *parent);
    void (*undraw)(lv_obj_t *parent);

    /* 0-100 的实时电平（麦克风应用用）；NULL 则不画电平条。
     * 返回值直接映射到进度条长度，调用方负责夹到 0-100。 */
    rt_uint8_t (*get_level)(void);
} app_entry_t;

/* 取应用表与数量。返回的数组是静态常量，调用方只读。 */
const app_entry_t *app_registry_get(int *count);

/* 启动某个应用，自动处理 mic0 互斥（会先停掉其他占用麦克风的应用）。
 * 返回 0=成功，负 errno=失败。 */
int app_registry_start(int index);

/* 停止某个应用 */
void app_registry_stop(int index);

#ifdef __cplusplus
}
#endif

#endif /* APP_REGISTRY_H */
