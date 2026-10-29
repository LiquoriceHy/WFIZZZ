/*
 * edge_shm.h - CM33 <-> CM55 共享内存布局（受控副本）
 *
 * 两工程各持一份本文件（M33: Edgi_Talk_M33_Driver_All/applications/，
 * M55: Edgi_Talk_M55_XiaoZhi/applications/），两工程之间没有共享 include
 * 机制；修改字段布局或偏移必须两侧同步修改，并用 diff 核对逐字节一致。
 *
 * 物理区：m33_m55_shared SOCMEM（256KB）。两核统一访问 0x261C0000：
 * 2026-09-28 真机验证 0x261C0000 与 0x061C0000 两个别名读出内容一致
 * （同一物理区），但 CM33 对 0x061C0000 的写操作会使总线挂死（读正常，
 * _C 疑为只读代码通路）；CM33 与 M55 对 0x261C0000 读写均正常。
 * 不要再按核拆分基址。
 * 两侧链接脚本均已定义该区：M55 侧 .reserved_socmem 整段 NOLOAD
 * 保留，M33 侧 .cy_shared_socmem 无人使用。
 *
 * 一致性约定：
 * - 控制行（M55 写 enable）与数据行（M33 写采样）分属两条 32B cache
 *   line，避免同一行上两核同时写入而互相干扰。
 * - M33 无 D-cache（__DCACHE_PRESENT 0），写完即落内存，无需 clean。
 * - M55 有 D-cache：读数据行前 INVALIDATE，写控制行后 FLUSH。
 * - 数据行 seq-lock：写侧 seq 置奇，写负载，DMB，seq 置偶；读侧两次
 *   seq 相等且为偶才算一次完整读。
 */
#ifndef __EDGE_SHM_H__
#define __EDGE_SHM_H__

#include <rtthread.h>

/* 两核统一基址：0x261C0000 读写皆正常；0x061C0000（_C 别名）CM33
 * 只能读不能写（写挂总线），勿用其发布数据。 */
#define EDGE_SHM_BASE        0x261C0000UL

#define EDGE_SHM_CTRL_OFF    0x00UL        /* 控制行：M55 拥有写权，M33 只读 */
#define EDGE_SHM_DATA_OFF    0x20UL        /* 数据行：M33 拥有写权，M55 只读 */

#define EDGE_SHM_LINE_SIZE   32UL          /* 与 M55 端口 __SCB_DCACHE_LINE_SIZE 一致 */

#define EDGE_SHM_AHT20_MAGIC 0x31485441UL  /* "AHT1"，发布端就绪标志 */

/* 读/写序屏障：跨核观察负载与 seq 的先后必须固定 */
#if defined(__GNUC__)
#define EDGE_SHM_DMB()   __asm__ volatile ("dmb sy" ::: "memory")
#else
#define EDGE_SHM_DMB()   __DMB()
#endif

/* 控制行：M55 -> M33 的请求（M33 侧监听线程轮询） */
typedef struct
{
    volatile rt_uint32_t enable;  /* M55 写 1 请求启动采集（幂等） */
    rt_uint8_t pad[EDGE_SHM_LINE_SIZE - sizeof(rt_uint32_t)];
} edge_shm_ctrl_t;

/* 数据行：M33 -> M55 的采样发布 */
typedef struct
{
    volatile rt_uint32_t magic;   /* EDGE_SHM_AHT20_MAGIC = 发布端已起 */
    volatile rt_uint32_t seq;     /* seq-lock 计数，奇 = 写入中 */
    volatile rt_int16_t  temp_x10;/* 0.1 ℃ */
    volatile rt_uint16_t hum_x10; /* 0.1 %RH */
    volatile rt_uint32_t tick_ms; /* M33 侧采样时刻，诊断用 */
    rt_uint8_t pad[EDGE_SHM_LINE_SIZE - 3 * sizeof(rt_uint32_t) - 2 * sizeof(rt_uint16_t)];
} edge_shm_data_t;

/* 布局守卫：两行必须各自恰好占满一条 cache line */
typedef char edge_shm_ctrl_size_check[(sizeof(edge_shm_ctrl_t) == EDGE_SHM_LINE_SIZE) ? 1 : -1];
typedef char edge_shm_data_size_check[(sizeof(edge_shm_data_t) == EDGE_SHM_LINE_SIZE) ? 1 : -1];

#endif /* __EDGE_SHM_H__ */
