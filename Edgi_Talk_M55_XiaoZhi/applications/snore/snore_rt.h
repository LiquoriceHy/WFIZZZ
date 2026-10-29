/*
 * snore_rt.h - 鼾声实时检测对外接口
 *
 * msh 命令 (`snore_rt start|stop|status|th|pub`) 与 UI 应用抽屉共用
 * 下面这几个函数，避免两套逻辑各自维护状态。
 */
#ifndef SNORE_RT_H
#define SNORE_RT_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    rt_bool_t   running;      /* 采集线程在跑 */
    rt_bool_t   mqtt_up;      /* paho 已连上 broker */
    rt_uint16_t threshold;    /* 0-100，对应概率 0.00-1.00 */
    rt_uint16_t last_prob;    /* 最近一窗的鼾声概率 0-100 */
    rt_uint32_t windows;      /* 已推理窗数 */
    rt_uint32_t events;       /* 已上报事件数 */
    rt_uint32_t rms;          /* 最近一块 PCM 的 RMS 电平（0-32767） */
} snore_rt_status_t;

/* 读一份状态快照（各字段 volatile，读到的组合可能略有偏差，UI 展示足够） */
void snore_rt_get_status(snore_rt_status_t *st);

/* 启动采集+推理线程。已在运行时返回 0（幂等）。失败返回负 errno。
 * 注意：调用方负责处理 mic0 互斥（见 app_registry.c）。 */
int snore_rt_start(void);

/* 请求停止（异步：置标志后线程自行退出，可轮询 get_status().running） */
void snore_rt_stop(void);

/* 设置阈值 0-100（>100 会被夹到 100） */
void snore_rt_set_threshold(rt_uint16_t th);

/* 手动发一条测试事件到 MQTT（验证链路用） */
void snore_rt_publish_test(void);

/* ---------------- 事件历史（抽屉详情页用） ---------------- */

#define SNORE_RT_HIST_MAX   8

typedef struct
{
    rt_uint32_t uptime_s;   /* 事件发生时的开机秒数 */
    rt_uint16_t prob;       /* 0-100 */
    rt_uint16_t cost_ms;    /* fe + infer 耗时 */
} snore_rt_hist_t;

/* 按时间倒序（最新在前）读出历史，返回实际条目数。
 * 无事件时返回 0，buf 内容不动。 */
int snore_rt_get_history(snore_rt_hist_t *buf, int max);

#ifdef __cplusplus
}
#endif

#endif /* SNORE_RT_H */
