/*
 * snore_ml_rtos_port.c - mtb_ml 的 RT-Thread OS 抽象层
 *
 * 背景：mtb_ml_ethosu.c 在 CY_RTOS_AWARE 下（本工程由 rtconfig_preinc.h
 * 全局定义）不再使用 Ethos-U 驱动自带的裸机 mutex/semaphore 桩，而是要求
 * 应用层提供：
 *
 *     mtb_ml_mutex_create / lock / unlock / destroy
 *     mtb_ml_sem_create / take / destroy
 *
 * 关键约束：同一文件中的 ethosu_semaphore_give() 为强符号，它将句柄强转为
 * cy_semaphore_t 并调用 cy_rtos_set_semaphore()。cy_semaphore_t 在 FreeRTOS
 * 后端中即 SemaphoreHandle_t。因此 create 必须产出 cy_semaphore_t，
 * 否则 give 会将 RT-Thread 的 rt_sem_t 当作 FreeRTOS 队列解引用，
 * 实测表现为 xQueueGenericSend 内部总线故障，BFAR 落在信号量名字符串上。
 *
 * 因此 sem 一侧直接复用官方 abstraction-rtos 的 cy_rtos_semaphore_*，
 * 保证 create/take/give 操作同一类对象。mutex 一侧不经过 cy_rtos_* 强转，
 * 使用 RT-Thread 原生 rt_mutex 更为直接。
 */

#include <rtthread.h>
#include <stdint.h>
#include <string.h>

#include "cyabs_rtos.h"
#include "cyabs_rtos_impl.h"

#ifndef ETHOSU_SEMAPHORE_WAIT_FOREVER
#define ETHOSU_SEMAPHORE_WAIT_FOREVER UINT64_MAX
#endif

#ifndef RT_USING_MUTEX
#error "mtb_ml RT-Thread port requires RT_USING_MUTEX"
#endif

static rt_uint32_t g_ml_index;

static void ml_make_name(const char *prefix, char *name, rt_size_t size)
{
    rt_base_t level;
    rt_uint32_t index;

    level = rt_hw_interrupt_disable();
    index = g_ml_index++;
    rt_hw_interrupt_enable(level);

    rt_snprintf(name, size, "%s%u", prefix, (unsigned int)(index % 1000U));
}

/* ---------------- mutex ---------------- */

void *mtb_ml_mutex_create(void)
{
    char name[RT_NAME_MAX];

    ml_make_name("mlm", name, sizeof(name));
    return (void *)rt_mutex_create(name, RT_IPC_FLAG_PRIO);
}

int mtb_ml_mutex_lock(void *mutex)
{
    if (mutex == RT_NULL)
        return -1;

    return (rt_mutex_take((rt_mutex_t)mutex, RT_WAITING_FOREVER) == RT_EOK)
               ? 0 : -1;
}

int mtb_ml_mutex_unlock(void *mutex)
{
    if (mutex == RT_NULL)
        return -1;

    return (rt_mutex_release((rt_mutex_t)mutex) == RT_EOK) ? 0 : -1;
}

void mtb_ml_mutex_destroy(void *mutex)
{
    if (mutex != RT_NULL)
        (void)rt_mutex_delete((rt_mutex_t)mutex);
}

/* ---------------- semaphore ---------------- */

/*
 * 句柄内存由这里自己管：cy_rtos_semaphore_init() 要求调用方给出
 * cy_semaphore_t 的存储位置，它只负责在里面写 xSemaphoreCreateCounting
 * 返回的句柄。所以返回给调用方的是这块存储的首地址，与
 * ethosu_semaphore_give() 里的强转保持一致。
 */
void *mtb_ml_sem_create(void)
{
    cy_semaphore_t *sem;

    sem = (cy_semaphore_t *)rt_malloc(sizeof(cy_semaphore_t));
    if (sem == RT_NULL)
        return RT_NULL;

    if (cy_rtos_semaphore_init(sem, 0xFFFF, 0) != CY_RSLT_SUCCESS)
    {
        rt_free(sem);
        return RT_NULL;
    }
    return (void *)sem;
}

int mtb_ml_sem_take(void *sem, uint64_t timeout)
{
    uint64_t timeout_ms;

    if (sem == RT_NULL)
        return -1;

    /* cy_rtos_semaphore_get 的 timeout 单位是毫秒，0 表示不等待；
     * UINT64_MAX 需要映射成 CY_RTOS_NEVER_TIMEOUT。 */
    if (timeout == ETHOSU_SEMAPHORE_WAIT_FOREVER)
        timeout_ms = CY_RTOS_NEVER_TIMEOUT;
    else
        timeout_ms = timeout;

    return (cy_rtos_semaphore_get((cy_semaphore_t *)sem, timeout_ms)
                == CY_RSLT_SUCCESS)
               ? 0 : -1;
}

void mtb_ml_sem_destroy(void *sem)
{
    if (sem == RT_NULL)
        return;

    (void)cy_rtos_semaphore_deinit((cy_semaphore_t *)sem);
    rt_free(sem);
}
