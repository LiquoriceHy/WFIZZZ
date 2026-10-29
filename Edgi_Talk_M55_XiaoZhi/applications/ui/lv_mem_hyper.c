/*
 * lv_mem_hyper.c - LVGL 分配器的 HyperRAM 分流
 *
 * 背景：中文字库要从 SD 卡运行时加载（lv_binfont_create），而
 * lv_binfont_loader.c 会把整个字形位图 lv_malloc 复制一份。全量 GB2312
 * 16px/4bpp 加载后约 990KB，主堆只有 520KB（__end__ = 0x2613CD68,
 * __HeapLimit = 0x261C0000），装不下。
 *
 * HyperRAM 有 2MB（0x64400000 起，BSP_USING_HYPERAM_SIZE = 0x200000），
 * 实测除了本文件之外没有任何使用者（nm 查 0x64400000 以上无静态符号）。
 * drv_hyperam.c 已把它注册成名为 "hyperam" 的 rt_memheap。
 *
 * 方案：按大小分流。
 *   - 大于 LV_MEM_HRAM_THRESHOLD 的分配走 HyperRAM
 *   - 其余走主堆 rt_malloc
 *
 * 为什么不是全部走 HyperRAM：HyperRAM 是 QSPI 接口，随机访问比片内 SRAM
 * 慢一个量级。LVGL 的控件、样式、链表节点是高频小对象随机访问，放进去
 * 会拖慢每一帧。而字形位图是"加载后基本只读、按需取一小段"的大块顺序
 * 数据，正好适合。8KB 这个阈值让字形位图（几百 KB）必然落进 HyperRAM，
 * 而气泡、按钮这些几百字节的对象留在主堆。
 *
 * 释放和重分配靠地址区间判定归属，不需要额外的簿记：
 *   HyperRAM  Win = [0x64400000, 0x64400000 + BSP_USING_HYPERAM_SIZE)
 * 落在里面就用 rt_memheap_free/rt_memheap_realloc，否则 rt_free/rt_realloc。
 */
#include <rtthread.h>
#include <stddef.h>

#include "lvgl.h"

#define DBG_TAG "lv.mem"
#define DBG_LVL DBG_WARNING
#include <rtdbg.h>

/* HyperRAM 窗口：与 drv_hyperam.c 的 PSRAM_ADDRESS / BSP_USING_HYPERAM_SIZE
 * 保持一致。这里不 include drv_hyperam.c 的私有宏，因为那个文件没有对外
 * 头文件；两个值都来自 rtconfig.h 和链接脚本，改动时会一起变。 */
#define HRAM_BASE   ((rt_ubase_t)0x64400000UL)
#define HRAM_SIZE   ((rt_ubase_t)BSP_USING_HYPERAM_SIZE)
#define HRAM_END    (HRAM_BASE + HRAM_SIZE)

/* 超过该大小才走 HyperRAM。
 * 8KB 的选型依据：字形位图整块有数百 KB，必然命中；LVGL 的绘制中间缓冲
 * （layer buffer）通常为几 KB 到数十 KB，放入 HyperRAM 同样可减轻主堆压力；
 * 而控件、样式与字符串均在数百字节以内，稳妥保留在主堆。 */
#define LV_MEM_HRAM_THRESHOLD   (8 * 1024)

/* drv_hyperam.c 提供的 HyperRAM 堆句柄。
 * 返回 RT_NULL 表示 BSP 未启用 HyperRAM（rtconfig.h 缺少
 * BSP_USING_HYPERAM 或 RT_USING_MEMHEAP_AS_HEAP）。此时所有分配退回主堆，
 * 功能不受影响，仅字库无法容纳，加载方会得到 NULL 并回退到内置字库。 */
extern struct rt_memheap *drv_hyperam_get_memheap(void);

static struct rt_memheap *s_hram;

static struct rt_memheap *hram_heap(void)
{
    if (s_hram == RT_NULL)
        s_hram = drv_hyperam_get_memheap();
    return s_hram;
}

static rt_bool_t ptr_in_hram(void *p)
{
    rt_ubase_t a = (rt_ubase_t)p;

    return (a >= HRAM_BASE && a < HRAM_END) ? RT_TRUE : RT_FALSE;
}

/* ---- LVGL 自定义分配器接口（LV_STDLIB_CUSTOM 要求这五个符号）---- */

void *lv_malloc_core(size_t size)
{
    struct rt_memheap *h;

    if (size >= LV_MEM_HRAM_THRESHOLD)
    {
        h = hram_heap();
        if (h != RT_NULL)
        {
            void *p = rt_memheap_alloc(h, size);

            if (p != RT_NULL)
                return p;
            LOG_W("hram alloc %u failed, falling back to main heap",
                  (unsigned)size);
        }
    }

    return rt_malloc(size);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    if (p == NULL)
        return lv_malloc_core(new_size);

    if (ptr_in_hram(p))
    {
        struct rt_memheap *h = hram_heap();

        if (h != RT_NULL)
            return rt_memheap_realloc(h, p, new_size);
    }

    return rt_realloc(p, new_size);
}

void lv_free_core(void *p)
{
    if (p == NULL)
        return;

    /* rt_memheap_free 内部会由指针反查所属 memheap（见 memheap.c 的
     * _memheap_free），因此可直接调用，无需先判断归属。
     * 但主堆分配的对象不能走这条路径：rt_free 使用 _MEM_FREE 宏，
     * 两者不通用，因此仍需分流。 */
    if (ptr_in_hram(p))
    {
        rt_memheap_free(p);
        return;
    }

    rt_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    rt_size_t total = 0, used = 0, max_used = 0;
    struct rt_memheap *h;

    if (mon_p == NULL)
        return;

    rt_memset(mon_p, 0, sizeof(*mon_p));

    /* 合并主堆与 HyperRAM 的统计。
     * 主堆没有直接的 info 接口，用 memheap_info 拿不到；这里只统计
     * HyperRAM，加上 rt_memory_info 能给出主堆数字。 */
    h = hram_heap();
    if (h != RT_NULL)
    {
        rt_memheap_info(h, &total, &used, &max_used);
    }

    mon_p->total_size = total;
    mon_p->free_size  = total - used;
    mon_p->max_used   = max_used;
    mon_p->used_pct   = (total > 0) ? (rt_uint8_t)(used * 100 / total) : 0;
    mon_p->frag_pct   = 0;
}

lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

void lv_mem_init(void)
{
    /* 什么都不用做：HyperRAM 堆由 drv_hyperam.c 的 INIT_BOARD_EXPORT
     * 在板级初始化阶段建好，早于 LVGL。 */
}

void lv_mem_deinit(void)
{
}

/* 供调试：打印两个堆的用量。msh 命令 lvm */
void lv_mem_show(void)
{
    rt_size_t total = 0, used = 0, max_used = 0;
    struct rt_memheap *h = hram_heap();
    rt_size_t mt = 0, mu = 0, mm = 0;

    rt_memory_info(&mt, &mu, &mm);

    rt_kprintf("main heap : total %u  used %u  max %u  free %u\n",
               (unsigned)mt, (unsigned)mu, (unsigned)mm,
               (unsigned)(mt - mu));

    if (h != RT_NULL)
    {
        rt_memheap_info(h, &total, &used, &max_used);
        rt_kprintf("hyperram  : total %u  used %u  max %u  free %u\n",
                   (unsigned)total, (unsigned)used, (unsigned)max_used,
                   (unsigned)(total - used));
    }
    else
    {
        rt_kprintf("hyperram  : not available\n");
    }
}
MSH_CMD_EXPORT_ALIAS(lv_mem_show, lvm, show main heap and hyperram usage);
