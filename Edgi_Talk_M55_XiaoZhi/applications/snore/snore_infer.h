/*
 * snore_infer.h - 鼾声检测推理接口（TFLM int8 YAMNet，mel patch 输入）
 *
 * 一次推理 = 15600 个 16kHz/16bit 单声道采样（0.975s）
 *   -> mel 前端（snore_frontend）得 96x64 log-mel patch
 *   -> int8 量化 -> TFLM 解释器 -> 521 类 sigmoid 概率（Snoring = 38）
 *
 * 用法：
 *   snore_infer_init()         分配 arena 并加载模型（进程内一次）
 *   snore_infer_window(...)    对一窗 PCM 推理，线程安全由调用方保证
 *   snore_infer_get_info(...)  查询 arena 落位/量化参数（调试用）
 */

#ifndef __SNORE_INFER_H__
#define __SNORE_INFER_H__

#include <rtdef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNORE_WIN_SAMPLES   15600   /* 0.975s @ 16kHz = 96 帧(160) + 400 窗长 */
#define SNORE_MEL_FRAMES    96
#define SNORE_MEL_BANDS     64
#define SNORE_CLASS_SNORE   38      /* yamnet_class_map.csv 里的 Snoring */
#define SNORE_TOPN          5

typedef struct
{
    float snore;                        /* Snoring 概率 0~1 */
    float top_score[SNORE_TOPN];        /* top-5 概率 */
    rt_uint16_t top_idx[SNORE_TOPN];    /* top-5 类别号 */
    rt_uint32_t fe_ms;                  /* mel 前端耗时 */
    rt_uint32_t infer_ms;               /* 模型推理耗时 */
} snore_result_t;

typedef struct
{
    rt_uint32_t arena_size;             /* 申请到的 arena 字节数 */
    const void *arena_addr;             /* arena 地址（0x64xxxxxx=HyperRAM）*/
    float in_scale;                     /* 输入量化参数 */
    int   in_zp;
    float out_scale;                    /* 输出量化参数 */
    int   out_zp;
    int   ok;
} snore_info_t;

/* 0=成功；<0=失败（-1 内存不足，-2 模型/算子异常，-3 张量形状不符，-4 重复初始化） */
int snore_infer_init(void);

/* pcm: SNORE_WIN_SAMPLES 个 int16；out 非空。0=成功 */
int snore_infer_window(const rt_int16_t *pcm, snore_result_t *out);

void snore_infer_get_info(snore_info_t *info);

#ifdef __cplusplus
}
#endif

#endif /* __SNORE_INFER_H__ */
