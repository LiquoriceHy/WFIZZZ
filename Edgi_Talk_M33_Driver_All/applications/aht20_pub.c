/*
 * aht20_pub.c - AHT20 温湿度采集 + 硬件 IPC 发布（M33 侧）
 *
 * 板上 AHT20 挂 M33 软 I2C1（rtconfig.h: BSP_SOFT_I2C1_SCL/SDA_PIN
 * 75/74 = P9.3/P9.2），只有本核够得着；M55 侧同名 i2c1 是模板遗留
 * 引脚（P13.1/P13.2），无传感器。
 *
 * 传输走 BSP 硬件 IPC（drv_ipc，需 BSP_USING_IPC）：帧池位于各核
 * allocatable_shared 跨核窗口（link.ld 的 .cy_sharedmem 段），由
 * Cy_IPC_Pipe 门铃串行化，全程零 SOCMEM 直接访问——2026-09-28 实证
 * 双核并发访问 m33_m55_shared 端口会挂死 CM33（读探针正常、写探针
 * 挂死 0x061C0000、轮询期双核流量必挂），故弃用共享内存 seq-lock
 * 轮询方案（历史见 edge_shm.h 注释）。
 *
 * 启动模型：INIT_APP 起采集线程，aht10_init("i2c1") 成功后每 5s 经
 * ipc1 设备（CM33 pipe0 → CM55）发布一帧；M55 未运行时发送失败仅
 * 节流打印，M55 起来后自动恢复。无握手：M33 常发，M55 随收。
 *
 * 注意：msh 的 demo_aht20 与本模块都会 aht10_init 同一颗芯片，
 * 二者勿同时使用。
 */
#include <rtthread.h>
#include <rtdevice.h>

#include "aht10.h"
#include "drv_ipc.h"

#define PUB_WORK_STACK     2048
#define PUB_WORK_PRIO      25      /* 采集线程：慢速 I2C，不抢实时任务 */
#define PUB_PERIOD_MS      5000
#define PUB_RETRY_MS       10000   /* 传感器 init 失败重试间隔 */

/* AHT20 量程过滤（含包驱动失败哨兵：温度 -50/湿度 0） */
#define PUB_TEMP_MIN       (-40.0f)
#define PUB_TEMP_MAX       85.0f
#define PUB_HUM_MAX        100.0f

/* 2026-09-29: 湿度元件硬件损坏（CRC 验证的测量帧 20bit 恒 0），
 * 湿度显示固定 26%；换新模块后删除本宏、恢复 aht10_read_humidity 调用 */
#define PUB_HUM_FIXED      26.0f

#define PUB_FRAME_TAG      0xA020U /* 帧类型标记：温湿度帧 */

static rt_device_t s_ipc_tx = RT_NULL;
static rt_uint32_t s_seq = 0;
static rt_uint32_t s_fail_cnt = 0;
static rt_uint32_t s_bad_cnt = 0;

/* 一帧温湿度：channel[0]=temp_x10 [1]=hum_x10 [2:3]=tick_ms [4]=TAG */
static void pub_publish(float temp_c, float hum_pct, rt_uint32_t tick_ms)
{
    edge_rc_frame_t f;
    rt_int16_t t10 = (rt_int16_t)(temp_c * 10.0f + (temp_c < 0 ? -0.5f : 0.5f));

    rt_memset(&f, 0, sizeof(f));
    f.client_id  = CM33_IPC_PIPE_CLIENT_ID;
    f.role       = RC_ROLE_M33;
    f.magic      = RC_MAGIC_WORD;
    f.seq        = ++s_seq;
    f.channel[0] = (rt_uint16_t)t10;
    f.channel[1] = (rt_uint16_t)(hum_pct * 10.0f + 0.5f);
    f.channel[2] = (rt_uint16_t)(tick_ms & 0xFFFFU);
    f.channel[3] = (rt_uint16_t)(tick_ms >> 16);
    f.channel[4] = PUB_FRAME_TAG;
    f.checksum   = edge_rc_checksum(&f);

    if (rt_device_write(s_ipc_tx, 0, &f, 1) != 1)
    {
        if ((s_fail_cnt++ % 12) == 0)   /* 首次即报，之后约每分钟一次 */
            rt_kprintf("[aht20pub] ipc write failed (M55 not up?)\n");
    }
    else if (s_fail_cnt != 0)
    {
        s_fail_cnt = 0;
        rt_kprintf("[aht20pub] ipc write recovered\n");
    }
}

static void pub_worker_entry(void *parameter)
{
    aht10_device_t dev = RT_NULL;

    RT_UNUSED(parameter);

    while (dev == RT_NULL)
    {
        dev = aht10_init("i2c1");
        if (dev == RT_NULL)
        {
            rt_kprintf("[aht20pub] aht10 init on i2c1 failed, retry in %ds\n",
                       PUB_RETRY_MS / 1000);
            rt_thread_mdelay(PUB_RETRY_MS);
        }
    }
    rt_kprintf("[aht20pub] aht20 on i2c1 ready, publish every %ds\n",
               PUB_PERIOD_MS / 1000);

    /* IPC 链路验证：等 cm55boot(+5s) 拉起的 M55 完成设备注册后，
     * 发 3 帧测试数据（25.0℃/50.0%），UI 显示即链路通（诊断用，验证后可删） */
    rt_thread_mdelay(7000);
    {
        int i;
        for (i = 0; i < 3; i++)
        {
            pub_publish(25.0f, 50.0f, rt_tick_get_millisecond());
            rt_thread_mdelay(300);
        }
        rt_kprintf("[aht20pub] ipc test frames sent (25.0/50.0)\n");
    }

    while (1)
    {
        float temp_c;
        float hum_pct;

        /* 2026-09-29: 湿度元件硬件损坏（CRC 验证的测量帧 20bit 恒 0），
         * 湿度显示固定 26%；换新模块后删掉本行、恢复 aht10_read_humidity 调用 */
        hum_pct = PUB_HUM_FIXED;

        /* 温度通道正常，继续真实采样（湿度不再发起 I2C 事务） */
        temp_c = aht10_read_temperature(dev);

        if (temp_c >= PUB_TEMP_MIN && temp_c <= PUB_TEMP_MAX)
        {
            s_bad_cnt = 0;
            pub_publish(temp_c, hum_pct, rt_tick_get_millisecond());
        }
        else if ((s_bad_cnt++ % 12) == 0)   /* 首次即报，之后每分钟一次 */
        {
            rt_kprintf("[aht20pub] temp out of range: t10=%d\n",
                       (int)(temp_c * 10.0f));
        }

        rt_thread_mdelay(PUB_PERIOD_MS);
    }
}

static int aht20_pub_start(void)
{
    rt_thread_t tid;

    if (edge_ipc_device_register() != RT_EOK)
    {
        rt_kprintf("[aht20pub] ipc device register failed\n");
        return -RT_ERROR;
    }
    s_ipc_tx = edge_ipc_device_find(EDGE_IPC0_DEVICE_NAME);   /* "ipc1"：CM33→CM55 */
    if (s_ipc_tx == RT_NULL)
    {
        rt_kprintf("[aht20pub] ipc tx device not found\n");
        return -RT_ERROR;
    }
    if (rt_device_open(s_ipc_tx, RT_DEVICE_OFLAG_RDWR) != RT_EOK)
    {
        rt_kprintf("[aht20pub] ipc tx device open failed\n");
        return -RT_ERROR;
    }

    tid = rt_thread_create("aht20pub", pub_worker_entry, RT_NULL,
                           PUB_WORK_STACK, PUB_WORK_PRIO, 10);
    if (tid == RT_NULL)
    {
        rt_kprintf("[aht20pub] worker thread create failed\n");
        return -RT_ERROR;
    }
    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(aht20_pub_start);
