/*
 * note.h - 便签/待办（4 条，可由 AI 语音写入）
 *
 * 设计约束（均为本工程实测结论，修改前请先阅读）：
 *
 * 1) 界面文案必须为 ASCII。lv_conf.h 的 LV_FONT_SIMSUN_16_CJK 仅含 1118 个
 *    稀疏汉字（实测缺少 签/办/闹/钟/个 等常用字），而界面正文使用纯 ASCII
 *    的 montserrat_16。因此卡片与列表中的固定文案全部为英文。
 *
 * 2) 便签内容本身可以是中文（UTF-8 原样存储、原样写入 JSON），
 *    但在屏上会显示为空白，这是本期已知限制，用户已确认接受。
 *    存储层不得因为无法显示就丢弃或改写内容，因为 AI 读回后仍需使用。
 *
 * 3) 所有写操作都不落盘，只修改内存并调用 data_store_mark_dirty()。
 *    落盘由 app_data 的 1 秒线程统一执行（见 data_store.h 的说明）。
 *    原因：MCP 工具回调运行在 websocket 接收线程中，同步写 SD 会阻塞音频上行。
 *
 * 4) 容量固定为 4 条。已满时返回 -RT_ENOSPC，由调用方（AI 工具）转为
 *    "便签已满，请先清除一条"的提示，而不是静默丢弃最旧的一条，
 *    因为静默丢弃会让用户误以为已记录成功。
 */
#ifndef __NOTE_H__
#define __NOTE_H__

#include <rtthread.h>

#define NOTE_MAX       4
#define NOTE_TEXT_LEN  64      /* 单条上限，含结尾 '\0'。约 20 个汉字 */

typedef struct
{
    char      text[NOTE_TEXT_LEN];   /* UTF-8，空串=空槽 */
    rt_bool_t done;
} note_item_t;

/* 开机从 SD 读回。由 data_store_load() 内部调用，应用层不必直接调。 */
void note_init(void);

/* 取第 idx 条的副本。越界或空槽返回 RT_FALSE。
 *
 * 为什么返回副本而不是内部指针：note 的写方来自三个线程（MCP 的
 * websocket 线程调 note_add、UI 的 LVGL 线程调 note_mark/note_del、
 * 开机加载线程调 note_restore），读方也在 LVGL 线程。返回内部指针
 * 会让调用方在锁外读 s_items[idx].text，可能读到 memcpy 到一半的
 * 字符串（中文 UTF-8 会显示成乱码方块）。
 *
 * out 长度至少 NOTE_TEXT_LEN；done 可为 RT_NULL。 */
rt_bool_t note_get_copy(int idx, char *out, rt_size_t len, rt_bool_t *done);

/* 已用的条数（含已完成的）。 */
int note_count(void);

/* 追加一条。成功返回新条目的下标（0..NOTE_MAX-1），
 * 已满返回 -RT_ENOSPC，text 为空返回 -RT_EINVAL。
 * 超长内容按 UTF-8 字符边界截断（不会切出半个汉字），不返回错误。 */
int note_add(const char *text);

/* 覆盖第 idx 条的内容。返回 0 成功。 */
int note_set(int idx, const char *text);

/* 设置完成标记。返回 0 成功。 */
int note_mark(int idx, rt_bool_t done);

/* 删除第 idx 条，后面的条目前移（保持列表紧凑，卡片显示不会留空洞）。 */
int note_del(int idx);

/* 清掉所有已完成的条目。返回删除条数。 */
int note_clear_done(void);

/* 清空全部。返回删除条数。 */
int note_clear_all(void);

/* ---- 以下供 data_store 使用，应用层不要调 ---- */

/* 直接写入某槽位，不校验、不置脏。data_store_load 专用。
 * data_store 会先把整个文件解析到临时数组、确认无误再逐条调本函数，
 * 所以这里不需要（也不该）负责"清空旧数据"。 */
void note_restore(int idx, const char *text, rt_bool_t done);

#endif /* __NOTE_H__ */
