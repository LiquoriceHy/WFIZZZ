/*
 * note.c - 便签/待办的存储实现
 *
 * 全部操作都在临界区内修改 s_items：MCP 工具回调运行于 websocket 线程，
 * UI 读取运行于 LVGL 线程，两者存在并发冲突。此处使用临界区而非互斥量，
 * 因为每个操作都是几字节的 memcpy，没有阻塞点，进入临界区不影响实时性。
 *
 * 写操作结束后统一调用 data_store_mark_dirty()，不直接落盘。
 */

#include <rtthread.h>
#include <string.h>

#include "note.h"
#include "data_store.h"

#define DBG_TAG "note"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

static note_item_t s_items[NOTE_MAX];

/* 按 UTF-8 字符边界截断。
 *
 * 为什么不能用 rt_strncpy：它会从中间切断多字节序列，留下一个孤立的
 * 首字节（如 0xE4），LVGL 和 JSON 都会把它当非法序列。中文便签必然
 * 走到这条路径（64 字节约 20 个汉字，AI 很容易说超）。
 *
 * 做法：先按字节长度截，然后回退到最近的 UTF-8 起始字节之前。
 * UTF-8 续字节的特征是高两位为 10（0x80-0xBF）。 */
static void note_copy_text(char *dst, const char *src, rt_size_t cap)
{
    rt_size_t len = rt_strlen(src);

    if (len >= cap)
    {
        rt_size_t cut = cap - 1;   /* 预留结尾 '\0' */

        /* 回退到字符边界：跳过所有续字节 */
        while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80)
            cut--;

        len = cut;
    }

    rt_memcpy(dst, src, len);
    dst[len] = '\0';
}

void note_init(void)
{
    rt_enter_critical();
    rt_memset(s_items, 0, sizeof(s_items));
    rt_exit_critical();
}

rt_bool_t note_get_copy(int idx, char *out, rt_size_t len, rt_bool_t *done)
{
    rt_bool_t got = RT_FALSE;

    if (idx < 0 || idx >= NOTE_MAX || out == RT_NULL || len == 0)
        return RT_FALSE;

    rt_enter_critical();
    /* 空槽对调用方等价于不存在 */
    if (s_items[idx].text[0] != '\0')
    {
        rt_strncpy(out, s_items[idx].text, len - 1);
        out[len - 1] = '\0';
        if (done != RT_NULL)
            *done = s_items[idx].done;
        got = RT_TRUE;
    }
    rt_exit_critical();

    return got;
}

int note_count(void)
{
    int i, n = 0;

    rt_enter_critical();
    for (i = 0; i < NOTE_MAX; i++)
    {
        if (s_items[i].text[0] != '\0')
            n++;
    }
    rt_exit_critical();

    return n;
}

/* 找第一个空槽，没有返回 -1 */
static int note_free_slot(void)
{
    int i;

    for (i = 0; i < NOTE_MAX; i++)
    {
        if (s_items[i].text[0] == '\0')
            return i;
    }
    return -1;
}

int note_add(const char *text)
{
    int slot;

    if (text == RT_NULL || text[0] == '\0')
        return -RT_EINVAL;

    rt_enter_critical();
    slot = note_free_slot();
    if (slot >= 0)
    {
        note_copy_text(s_items[slot].text, text, NOTE_TEXT_LEN);
        s_items[slot].done = RT_FALSE;
    }
    rt_exit_critical();

    if (slot < 0)
    {
        LOG_W("note full (%d)", NOTE_MAX);
        return -RT_ENOSPC;
    }

    LOG_I("add #%d (len=%u)", slot, (unsigned)rt_strlen(s_items[slot].text));
    data_store_mark_dirty();
    return slot;
}

int note_set(int idx, const char *text)
{
    if (idx < 0 || idx >= NOTE_MAX || text == RT_NULL)
        return -RT_EINVAL;

    rt_enter_critical();
    /* 空串等价于删除该条 */
    if (text[0] == '\0')
    {
        rt_memset(&s_items[idx], 0, sizeof(s_items[idx]));
    }
    else
    {
        note_copy_text(s_items[idx].text, text, NOTE_TEXT_LEN);
    }
    rt_exit_critical();

    data_store_mark_dirty();
    return 0;
}

int note_mark(int idx, rt_bool_t done)
{
    rt_bool_t empty;

    if (idx < 0 || idx >= NOTE_MAX)
        return -RT_EINVAL;

    rt_enter_critical();
    empty = (s_items[idx].text[0] == '\0');
    if (!empty)
        s_items[idx].done = done ? RT_TRUE : RT_FALSE;
    rt_exit_critical();

    if (empty)
        return -RT_EINVAL;

    data_store_mark_dirty();
    return 0;
}

int note_del(int idx)
{
    int i;

    if (idx < 0 || idx >= NOTE_MAX)
        return -RT_EINVAL;

    rt_enter_critical();
    if (s_items[idx].text[0] == '\0')
    {
        rt_exit_critical();
        return -RT_EINVAL;
    }

    /* 后续条目前移，保持紧凑：卡片只显示前两条，留空洞会让用户
     * 看到"第 1 条是空的但第 3 条有内容"这种费解状态。 */
    for (i = idx; i + 1 < NOTE_MAX; i++)
        s_items[i] = s_items[i + 1];
    rt_memset(&s_items[NOTE_MAX - 1], 0, sizeof(s_items[NOTE_MAX - 1]));
    rt_exit_critical();

    data_store_mark_dirty();
    return 0;
}

int note_clear_done(void)
{
    int i, n = 0;

    /* 在单次临界区内完成：不能先在外部统计，再逐个调用 note_del()，
     * 因为那些函数自身也会进入临界区（嵌套是允许的），但在外部读取
     * s_items 会与 MCP 线程的写操作并发。此处采用原地压实，避免多次
     * memmove。 */
    rt_enter_critical();
    for (i = 0; i < NOTE_MAX; i++)
    {
        if (s_items[i].text[0] != '\0' && s_items[i].done)
            n++;
    }

    if (n > 0)
    {
        int dst = 0;

        for (i = 0; i < NOTE_MAX; i++)
        {
            if (s_items[i].text[0] != '\0' && !s_items[i].done)
            {
                if (dst != i)
                    s_items[dst] = s_items[i];
                dst++;
            }
        }
        for (i = dst; i < NOTE_MAX; i++)
            rt_memset(&s_items[i], 0, sizeof(s_items[i]));
    }
    rt_exit_critical();

    if (n > 0)
    {
        LOG_I("cleared %d done note(s)", n);
        data_store_mark_dirty();
    }

    return n;
}

int note_clear_all(void)
{
    int n = note_count();

    note_init();
    if (n > 0)
    {
        LOG_I("cleared all %d note(s)", n);
        data_store_mark_dirty();
    }
    return n;
}

/* ---- data_store 专用 ---- */

void note_restore(int idx, const char *text, rt_bool_t done)
{
    if (idx < 0 || idx >= NOTE_MAX)
        return;

    /* 进临界区：本函数在 data_store_boot_task 里跑，而 LVGL 线程的卡片
     * timer 已经在读 s_items（1 秒一次），不加锁会读到拷贝一半的文本。 */
    rt_enter_critical();
    note_copy_text(s_items[idx].text, text ? text : "", NOTE_TEXT_LEN);
    s_items[idx].done = done ? RT_TRUE : RT_FALSE;
    rt_exit_critical();
}
