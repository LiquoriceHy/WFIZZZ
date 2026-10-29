/*
 * snore_frontend.h - YAMNet log-mel 前端（与 Google features.py 逐位对齐）
 *
 * 规格（从 float 波形版 YAMNet tflite 图内常量原样抽取，gen_headers.py 生成表）：
 *   400 点周期 Hann 窗 / hop 160 / 512 点 FFT / 幅度谱 |X|（非功率谱）
 *   64 带 mel 滤波器组（125~7500Hz）/ log(mel + 0.001)
 * 15600 采样 -> 恰好 96 帧 -> 输出 96x64 patch（行主序：patch[i*64+b]）
 *
 * 在树莓派上已用 float 模型图内 mel 输出头逐元素比对过（max diff 1.8e-5）。
 */

#ifndef __SNORE_FRONTEND_H__
#define __SNORE_FRONTEND_H__

#include <rtdef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SNORE_FE_WIN_LEN    400
#define SNORE_FE_HOP        160
#define SNORE_FE_FFT        512
#define SNORE_FE_SPEC_BINS  257     /* FFT/2 + 1 */

/* pcm: SNORE_WIN_SAMPLES 个 int16；patch: SNORE_MEL_FRAMES*SNORE_MEL_BANDS float。
   返回 0=成功。调用方需保证不被并发调用（FFT 缓冲为模块静态量）。 */
int snore_frontend_window(const rt_int16_t *pcm, float *patch);

#ifdef __cplusplus
}
#endif

#endif /* __SNORE_FRONTEND_H__ */
