/*
 * ethosu_rtos_port.cpp - Ethos-U55 驱动的 RT-Thread OS 适配层
 *
 * Ethos-U 驱动（porting/ethos-core-driver/src/ethosu_driver.c）在编入时
 * 要求外部提供一组 OS 原语：mutex / semaphore / irq 注册 / 推理钩子。
 * Arm 的参考实现面向裸机和 FreeRTOS，本文件是 RT-Thread 版本。
 *
 * 结构照抄 Infineon 官方例程 Edgi_Talk_M55_DEEPCRAFT_Deploy_Vision 的
 * applications/uvc_ai_ethosu_rtos.c（同 SDK、同硬件、已验证可用），
 * 只补上本工程需要的初始化入口。
 *
 * 放在 porting/rtthread/ 下会被 edge-impulse/SConscript 现有的
 * rtthread 目录 glob 自动收编，无需额外改构建脚本。
 */
#include <rtthread.h>
#include <stdint.h>

#include "ethosu_driver.h"

#ifndef ETHOSU_SEMAPHORE_WAIT_FOREVER
#define ETHOSU_SEMAPHORE_WAIT_FOREVER UINT64_MAX
#endif

#ifndef RT_USING_SEMAPHORE
#error "Ethos-U RT-Thread port requires RT_USING_SEMAPHORE"
#endif

static rt_uint32_t g_ethosu_ipc_index;

static void ethosu_make_name(const char *prefix, char *name, rt_size_t size)
{
    rt_base_t level;
    rt_uint32_t index;

    level = rt_hw_interrupt_disable();
    index = g_ethosu_ipc_index++;
    rt_hw_interrupt_enable(level);

    rt_snprintf(name, size, "%s%u", prefix, (unsigned int)(index % 1000U));
}

/* 毫秒超时 → RT-Thread tick；UINT64_MAX 表示永久等待 */
static rt_int32_t ethosu_timeout_to_ticks(uint64_t timeout)
{
    uint64_t ticks;

    if (timeout == ETHOSU_SEMAPHORE_WAIT_FOREVER)
        return RT_WAITING_FOREVER;

    if (timeout == 0U)
        return RT_WAITING_NO;

    ticks = (timeout * (uint64_t)RT_TICK_PER_SECOND + 999ULL) / 1000ULL;
    if (ticks == 0U)
        ticks = 1U;
    if (ticks > 0x7fffffffULL)
        return RT_WAITING_FOREVER;

    return (rt_int32_t)ticks;
}

/* ---------------- mutex ---------------- */

void *ethosu_mutex_create(void)
{
    char name[RT_NAME_MAX];

    ethosu_make_name("emu", name, sizeof(name));
    return (void *)rt_mutex_create(name, RT_IPC_FLAG_PRIO);
}

void ethosu_mutex_destroy(void *mutex)
{
    if (mutex != RT_NULL)
        (void)rt_mutex_delete((rt_mutex_t)mutex);
}

int ethosu_mutex_lock(void *mutex)
{
    if (mutex == RT_NULL)
        return -1;

    return (rt_mutex_take((rt_mutex_t)mutex, RT_WAITING_FOREVER) == RT_EOK) ? 0 : -1;
}

int ethosu_mutex_unlock(void *mutex)
{
    if (mutex == RT_NULL)
        return -1;

    return (rt_mutex_release((rt_mutex_t)mutex) == RT_EOK) ? 0 : -1;
}

/* ---------------- semaphore ---------------- */

void *ethosu_semaphore_create(void)
{
    char name[RT_NAME_MAX];

    ethosu_make_name("ese", name, sizeof(name));
    return (void *)rt_sem_create(name, 0, RT_IPC_FLAG_PRIO);
}

void ethosu_semaphore_destroy(void *sem)
{
    if (sem != RT_NULL)
        (void)rt_sem_delete((rt_sem_t)sem);
}

int ethosu_semaphore_take(void *sem, uint64_t timeout)
{
    rt_int32_t wait_ticks;

    if (sem == RT_NULL)
        return -1;

    wait_ticks = ethosu_timeout_to_ticks(timeout);

    /* 临时诊断：把"永久等待"降成 5 秒。NPU 完成中断若因故不来，驱动会
     * 走超时分支返回错误，而不是把调用它的 shell 线程永远挂住——后者
     * 会让整条串口失去响应，连排查信息都取不到。
     * 定位完成后应改回 PERMANENT 语义。 */
    if (wait_ticks == RT_WAITING_FOREVER)
        wait_ticks = rt_tick_from_millisecond(5000);

    if (rt_sem_take((rt_sem_t)sem, wait_ticks) != RT_EOK)
    {
        rt_kprintf("[ethosu] semaphore timeout\n");
        return -1;
    }
    return 0;
}

int ethosu_semaphore_give(void *sem)
{
    rt_err_t ret;

    if (sem == RT_NULL)
        return -1;

    ret = rt_sem_release((rt_sem_t)sem);
    if ((ret == RT_EOK) || (ret == -RT_EFULL))
    {
        /* 驱动偶尔会多给一次；保持与官方 RTOS 包装一致，不硬失败 */
        return 0;
    }

    return -1;
}

/* ---------------- 推理钩子 ---------------- */

/*
 * 驱动回调点（ethosu_driver.c）：
 *   373 行  ethosu_inference_begin()        <- 在 run_command_stream 之前
 *   376 行  ethosu_dev_run_command_stream() <- 这里才写 QBASE/QSIZE/BASEP
 *   591 行  ethosu_inference_end()          <- wait 返回之后
 *
 * 所以读"NPU 实际拿到的地址"只能放在 end 里；放在 begin 里读到的一定是
 * 上一轮的残留值（首次推理时全是 0）。
 *
 * end 里同时把 QREAD 打出来：它是 NPU 已经消费掉的 command stream 字节数。
 * QREAD==0 表示核心一个字节都没读，QREAD>0 但未完表示读到中途卡住。
 */
void ethosu_inference_begin(struct ethosu_driver *drv, void *user_arg)
{
    RT_UNUSED(drv);
    RT_UNUSED(user_arg);
    rt_kprintf("[ethosu] begin\n");
}

void ethosu_inference_end(struct ethosu_driver *drv, void *user_arg)
{
    RT_UNUSED(user_arg);
    if (drv != RT_NULL && drv->dev.reg != RT_NULL)
    {
        volatile uint32_t *npu =
            (volatile uint32_t *)(uintptr_t)drv->dev.reg;

        /* BASEP 每项占 8 字节：低 32 位是地址，高 32 位保留（U55 只用低半）。
         * 所以 basepN 的偏移是 0x80 + 8*N，而不是 0x80 + 4*N——按后者的
         * 写法读到的其实是前一项的保留字（恒 0），曾因此误判 basep1 为 0。 */
        rt_kprintf("[ethosu] end qbase=0x%08x qsize=%u qread=0x%08x "
                   "basep0=0x%08x basep1=0x%08x basep2=0x%08x basep3=0x%08x\n",
                   (unsigned)npu[0x10 / 4], (unsigned)npu[0x20 / 4],
                   (unsigned)npu[0x18 / 4],
                   (unsigned)npu[(0x80 + 8 * 0) / 4],
                   (unsigned)npu[(0x80 + 8 * 1) / 4],
                   (unsigned)npu[(0x80 + 8 * 2) / 4],
                   (unsigned)npu[(0x80 + 8 * 3) / 4]);
    }
    else
    {
        rt_kprintf("[ethosu] end\n");
    }
}
