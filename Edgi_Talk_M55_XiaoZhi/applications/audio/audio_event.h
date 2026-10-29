/*
 * audio_event.h - 音频电平事件检测对外接口
 *
 * 与 msh 命令共用同一套状态与启停逻辑，供 UI 应用抽屉调用。
 * 注意：本模块与 snore_rt 都会打开 mic0，不能同时运行；
 * 互斥由 app_registry.c 在启动前处理。
 */
#ifndef AUDIO_EVENT_H
#define AUDIO_EVENT_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    rt_bool_t   running;        /* 采集线程在跑 */
    rt_bool_t   mqtt_started;   /* paho 客户端已创建 */
    rt_bool_t   mqtt_connected; /* 已连上 broker */
    rt_uint32_t rms_last;       /* 最近一块的 RMS 电平 */
    rt_uint32_t threshold;      /* 事件阈值（RMS） */
    rt_uint32_t events;         /* 已触发事件数 */
} audio_event_status_t;

void audio_event_get_status(audio_event_status_t *st);

/* 启动采集线程（幂等）。失败返回负 errno。 */
int audio_event_start(void);

/* 请求停止（异步） */
void audio_event_stop(void);

/* 设置 RMS 阈值 */
void audio_event_set_threshold(rt_uint32_t th);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_EVENT_H */
