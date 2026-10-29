/*
 * cjk_font.c - 从 SD 卡运行时加载中文字库
 *
 * 不使用编译期字库的原因：LVGL 自带的 lv_font_simsun_16_cjk 仅含 1000 个
 * 常用字，实测缺少"签/办/闹/钟/设/页/扫/无/线"等界面高频字，导致中文文案
 * 整段不显示；而将全量 GB2312（6763 字）编译进 flash 后，运行时
 * lv_binfont_loader 仍会复制一份到堆中，主堆 520KB 无法容纳。
 *
 * 因此改为：字库 bin 存放于 SD 卡，开机时读入 HyperRAM（2MB，Dummy 占用），
 * 再通过 lv_binfont_create_from_buffer 由该内存创建字体对象。
 *
 * 内存路径：
 *   1. rt_memheap_alloc(&hyperam, 文件大小)：显式指定 HyperRAM，
 *      不使用 rt_malloc（该接口仅使用主堆）。
 *   2. fread 整块读入。
 *   3. lv_binfont_create_from_buffer(buf, size)：该接口并非零拷贝，
 *      lv_binfont_loader.c 会将字形位图、cmap、glyph_dsc 逐项 lv_malloc
 *      复制一份；这些分配（每块均远大于 8KB）由 lv_mem_hyper.c 的分流器
 *      接收并落至 HyperRAM，因此不占用主堆。
 *   4. 复制完成后即可释放原始 bin 缓冲。
 *
 * 失败回退：SD 卡未插、文件不存在或格式错误时，一律退回编译期的
 * lv_font_simsun_16_cjk，界面仍保持"部分中文 + 完整 ASCII"，不会空白。
 */
#include <rtthread.h>
#include <stdio.h>
#include <string.h>
#include <dfs_posix.h>

#include "lvgl.h"
#include "ui_theme.h"
#include "cjk_font.h"

#define DBG_TAG "cjk.font"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 字库文件位置。存放于 SD 卡而非 flash，理由见文件头。
 * 文件名对应 sdcard/wfizzz/ 下生成的 bin。 */
#ifndef CJK_FONT_PATH
#define CJK_FONT_PATH   "/sdcard/wfizzz/cjk16.bin"
#endif

/* 读文件时的块大小。SD 卡顺序读 4KB 一块效率较好，
 * 更大的块需要从余量有限的主堆中切出临时缓冲。 */
#define CJK_READ_CHUNK  4096

extern struct rt_memheap *drv_hyperam_get_memheap(void);

/* 加载成功后的字体对象；RT_NULL 表示用内置字库 */
static lv_font_t *s_cjk_font;
static rt_bool_t  s_tried;

lv_font_t *cjk_font_get(void)
{
    return s_cjk_font;
}

rt_bool_t cjk_font_ready(void)
{
    return (s_cjk_font != RT_NULL) ? RT_TRUE : RT_FALSE;
}

/* 把整个文件读进 HyperRAM。成功返回缓冲区，失败返回 RT_NULL。 */
static void *load_file_to_hram(const char *path, rt_size_t *out_size)
{
    struct rt_memheap *h;
    FILE *fp;
    rt_size_t total = 0, got = 0;
    void *buf;

    *out_size = 0;

    fp = fopen(path, "rb");
    if (fp == RT_NULL)
    {
        LOG_W("open %s failed (SD 卡未插或未拷字库)", path);
        return RT_NULL;
    }

    /* 求文件大小：DFS v1 没有可靠的 fstat 取 st_size 路径，
     * 用 fseek 到末尾再 ftell。 */
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        LOG_W("seek end failed");
        fclose(fp);
        return RT_NULL;
    }
    {
        long sz = ftell(fp);

        if (sz <= 0)
        {
            LOG_W("bad file size %ld", sz);
            fclose(fp);
            return RT_NULL;
        }
        total = (rt_size_t)sz;
    }
    if (fseek(fp, 0, SEEK_SET) != 0)
    {
        LOG_W("seek set failed");
        fclose(fp);
        return RT_NULL;
    }

    h = drv_hyperam_get_memheap();
    if (h == RT_NULL)
    {
        LOG_E("HyperRAM heap unavailable, cannot load CJK font");
        fclose(fp);
        return RT_NULL;
    }

    /* 显式向 HyperRAM 申请。rt_malloc 仅使用主堆，此处必须使用 memheap。 */
    buf = rt_memheap_alloc(h, total);
    if (buf == RT_NULL)
    {
        LOG_E("HyperRAM alloc %u failed", (unsigned)total);
        fclose(fp);
        return RT_NULL;
    }

    /* 整块读。分块的原因是 FatFs 单次读长度使用 UINT，且出错时可获知
     * 已读长度，便于诊断。 */
    while (got < total)
    {
        rt_size_t want = total - got;
        rt_size_t n;

        if (want > CJK_READ_CHUNK)
            want = CJK_READ_CHUNK;

        n = fread((rt_uint8_t *)buf + got, 1, want, fp);
        if (n == 0)
        {
            LOG_E("fread stalled at %u/%u", (unsigned)got, (unsigned)total);
            break;
        }
        got += n;
    }
    fclose(fp);

    if (got != total)
    {
        rt_memheap_free(buf);
        return RT_NULL;
    }

    LOG_I("loaded %u bytes from %s", (unsigned)total, path);
    *out_size = total;
    return buf;
}

/* 异步加载线程。
 *
 * 不采用在 LVGL 线程内直接加载的方式，原因见 lv_port.c 的注释：
 * 重试最长 5 秒，阻塞在 LVGL 线程等同于界面冻结 5 秒。
 *
 * 优先级低于 LVGL（LVGL 约为 RT_THREAD_PRIORITY_MAX-2，此处取中间偏低
 * 的 20），以保证加载期间界面滑动不卡顿。 */
#define CJK_THREAD_STACK   3072
#define CJK_THREAD_PRIO    20

/* 把字体切换排到 LVGL 线程里执行。
 *
 * 跨线程访问 LVGL 会导致崩溃（2026-10-27 真机验证）：
 * 最初在 cjkfont 线程中直接调用 ui_theme_apply_cjk_font()，依据是
 * "lv_style_set_text_font 只修改结构体字段，invalidate 只置脏标记"。
 * 该判断是错误的：lv_obj_report_style_change() 会遍历屏幕树并对每个控件
 * 调用 lv_obj_refresh_style()，后者访问样式链、重算布局，并触发绘制相关
 * 的刷新逻辑，这些均属于 LVGL 线程独占的数据结构。
 *
 * 真机表现（hard fault on thread: LVGL）：
 *   usage fault: SCB_CFSR_UFSR:0x01 UNDEFINSTR
 *   pc: 0x000000b8   lr: 0x60a1cbdd
 *   lr 落在 lv_vg_lite_pending_remove_all() 内，即 GPU 渲染管线的
 *   待处理链表。cjkfont 线程修改样式时，LVGL 线程正在遍历该链表，
 *   读到半更新的指针后跳转到 0x000000b8（无效指令地址）。
 *
 * 正确做法：任何触碰 LVGL 对象或样式的操作都必须回到 LVGL 线程执行。
 * lv_async_call() 将回调放入 LVGL 异步队列，由 lv_timer_handler() 在
 * LVGL 线程上下文中取出执行。 */
static void cjk_apply_async_cb(void *user_data)
{
    RT_UNUSED(user_data);

    ui_theme_apply_cjk_font();
}

static void cjk_font_thread_entry(void *arg)
{
    RT_UNUSED(arg);

    if (cjk_font_init() == 0)
    {
        /* 排队到 LVGL 线程执行，不在本线程直接访问 UI。
         * 返回 LV_RESULT_OK 表示已入队；失败（内存不足）时字库
         * 仍已加载成功，仅本次不切换字体。下一帧不会自动重试，
         * 但不会崩溃，且重启一次通常即可生效。 */
        if (lv_async_call(cjk_apply_async_cb, RT_NULL) != LV_RESULT_OK)
        {
            LOG_E("lv_async_call failed, font loaded but not applied");
        }
        else
        {
            LOG_I("font apply queued to LVGL thread");
        }
    }
}

void cjk_font_start_async(void)
{
    static rt_thread_t tid;

    if (tid != RT_NULL)
        return;

    tid = rt_thread_create("cjkfont", cjk_font_thread_entry, RT_NULL,
                           CJK_THREAD_STACK, CJK_THREAD_PRIO, 10);
    if (tid == RT_NULL)
    {
        LOG_E("create cjkfont thread failed");
        return;
    }

    rt_thread_startup(tid);
}

int cjk_font_init(void)
{
    void *raw;
    rt_size_t size = 0;
    lv_font_t *font;
    int tries;

    if (s_tried)
        return (s_cjk_font != RT_NULL) ? 0 : -1;
    s_tried = RT_TRUE;

    /* 重试读文件。
     *
     * 时序问题（2026-10-27 真机验证）：lv_user_gui_init() 由 LVGL 线程在
     * main() 中调用，而 SD 卡挂载由 mnt.c 的 sd_hotplug 线程完成；该线程
     * 优先级为 RT_THREAD_PRIORITY_MAX - 2（最低档之一），且在等待卡检测。
     * 开机瞬间 /sdcard 通常尚未挂载，首次 fopen 必然失败。
     *
     * 早期实现只尝试一次并永久标记失败，导致字库始终不加载（lvm 显示
     * hyperram used 仅 114KB，远小于字库的 860KB）。
     *
     * 现为 5 秒内每 200ms 尝试一次。正常情况下第 1-3 次即成功；
     * 确实未插卡时 5 秒后放弃并回退内置字库，不影响开机。 */
    for (tries = 0; tries < 25; tries++)
    {
        raw = load_file_to_hram(CJK_FONT_PATH, &size);
        if (raw != RT_NULL)
            break;

        if (tries == 0)
            LOG_I("waiting for %s (SD mount may be in progress)", CJK_FONT_PATH);

        rt_thread_mdelay(200);
    }

    if (raw == RT_NULL)
    {
        LOG_W("CJK font not loaded after retries, falling back to Montserrat");
        return -1;
    }

    LOG_I("got %u bytes after %d tries", (unsigned)size, tries + 1);

    /* lv_binfont_create_from_buffer 需要 LV_USE_FS_MEMFS（lv_conf.h 已开）。
     * 该接口将 bin 解析为 lv_font_fmt_txt_dsc_t，所有数据经 lv_malloc
     * 复制，大块分配由 lv_mem_hyper.c 转入 HyperRAM。 */
    font = lv_binfont_create_from_buffer(raw, (uint32_t)size);

    /* 无论成败，原始 bin 均不再需要：成功时 LVGL 已复制，失败时同样无用。
     * 该 source 缓冲位于 HyperRAM，不释放将浪费一份内存。 */
    rt_memheap_free(raw);

    if (font == RT_NULL)
    {
        LOG_E("lv_binfont_create_from_buffer failed (bin 格式不对?)");
        return -1;
    }

    /* 设置 fallback 链：英文与数字使用原有的 Montserrat 渲染。
     *
     * 字库 bin 中仅有汉字，不含 ASCII（生成时未加 --range 0x20-0x7F）。
     * 若直接作为 UI_FONT_TEXT 使用，LVGL 遇到英文将找不到字形；配置
     * fallback 后，中文取自该字库，英文落至 Montserrat，两侧字形风格
     * 各自正确。
     *
     * 反向做法（将 ASCII 打进 CJK 字库）会使英文被黑体渲染，与界面其它
     * 仍使用 Montserrat 的位置（时钟、按钮等）风格不一致，整体观感突变。
     *
     * lv_font_t.fallback 为可写字段，lv_binfont_create_from_buffer 返回的
     * 对象由 malloc 分配（非 const），可直接赋值。 */
    font->fallback = &lv_font_montserrat_16;

    s_cjk_font = font;

    LOG_I("CJK font ready: %d px, line_height %d, fallback -> montserrat_16",
          (int)font->line_height, (int)font->line_height);

    return 0;
}

void cjk_font_info(void)
{
    rt_size_t total = 0, used = 0, max_used = 0;
    struct rt_memheap *h = drv_hyperam_get_memheap();

    rt_kprintf("----------------------------------------\n");
    rt_kprintf("CJK font\n");
    rt_kprintf("  path   : %s\n", CJK_FONT_PATH);
    rt_kprintf("  status : %s\n",
               s_cjk_font ? "loaded (runtime, HyperRAM)"
                          : (s_tried ? "failed -> using Montserrat (ASCII only)"
                                     : "not attempted"));
    if (s_cjk_font)
    {
        rt_kprintf("  size   : line_height %d px\n",
                   (int)s_cjk_font->line_height);
    }
    if (h != RT_NULL)
    {
        rt_memheap_info(h, &total, &used, &max_used);
        rt_kprintf("  hram   : total %u  used %u  free %u\n",
                   (unsigned)total, (unsigned)used,
                   (unsigned)(total - used));
    }
    rt_kprintf("----------------------------------------\n");
}
MSH_CMD_EXPORT_ALIAS(cjk_font_info, cjk, show CJK font status);
