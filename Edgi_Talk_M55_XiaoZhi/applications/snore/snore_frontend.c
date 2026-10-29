/*
 * snore_frontend.c - YAMNet log-mel 前端实现
 *
 * 前端常量（Hann 窗、mel 滤波器组）不是按公式现算的，而是从 float 波形版
 * YAMNet tflite 的图内常量张量里原样抽出来生成（snore_frontend_tables.h），
 * 保证与训练前端 bit 级一致。见 snore_check/mcu/gen_headers.py。
 */

#include <rtthread.h>
#include <string.h>
#include <math.h>
#include "snore_frontend.h"
#include "snore_infer.h"

/* 表只在本文件 include 一次 */
#include "snore_frontend_tables.h"

/* 旋转因子表：512 点 FFT 各级共用，stride = 512/len 取样 */
static float s_tw_re[SNORE_FE_FFT / 2];
static float s_tw_im[SNORE_FE_FFT / 2];
static float s_fft_re[SNORE_FE_FFT];
static float s_fft_im[SNORE_FE_FFT];
static float s_mag[SNORE_FE_SPEC_BINS];
static rt_bool_t s_tw_ready = RT_FALSE;

static void twiddle_init(void)
{
    int k;

    if (s_tw_ready)
        return;
    for (k = 0; k < SNORE_FE_FFT / 2; k++)
    {
        float ang = -2.0f * 3.14159265358979f * k / SNORE_FE_FFT;
        s_tw_re[k] = cosf(ang);
        s_tw_im[k] = sinf(ang);
    }
    s_tw_ready = RT_TRUE;
}

/* 迭代式 radix-2 FFT，512 点，原地，输入 im 全零 */
static void fft_512(void)
{
    const int n = SNORE_FE_FFT;
    int i, j, len;

    for (i = 1, j = 0; i < n; i++)              /* 位反转重排 */
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
        {
            float t = s_fft_re[i]; s_fft_re[i] = s_fft_re[j]; s_fft_re[j] = t;
            t = s_fft_im[i]; s_fft_im[i] = s_fft_im[j]; s_fft_im[j] = t;
        }
    }

    for (len = 2; len <= n; len <<= 1)
    {
        int half = len >> 1;
        int stride = n / len;                   /* 旋转因子表取样步距 */
        for (i = 0; i < n; i += len)
        {
            for (j = 0; j < half; j++)
            {
                int a = i + j, b = a + half;
                float wr = s_tw_re[j * stride], wi = s_tw_im[j * stride];
                float tr = s_fft_re[b] * wr - s_fft_im[b] * wi;
                float ti = s_fft_re[b] * wi + s_fft_im[b] * wr;
                s_fft_re[b] = s_fft_re[a] - tr;
                s_fft_im[b] = s_fft_im[a] - ti;
                s_fft_re[a] += tr;
                s_fft_im[a] += ti;
            }
        }
    }
}

int snore_frontend_window(const rt_int16_t *pcm, float *patch)
{
    int frame, k, b;

    twiddle_init();

    for (frame = 0; frame < SNORE_MEL_FRAMES; frame++)
    {
        const rt_int16_t *src = pcm + frame * SNORE_FE_HOP;
        float *out = patch + frame * SNORE_MEL_BANDS;

        /* 加窗 + 零填充到 512 点 */
        for (k = 0; k < SNORE_FE_WIN_LEN; k++)
        {
            s_fft_re[k] = (float)src[k] * snore_hann[k] / 32768.0f;
            s_fft_im[k] = 0.0f;
        }
        for (; k < SNORE_FE_FFT; k++)
        {
            s_fft_re[k] = 0.0f;
            s_fft_im[k] = 0.0f;
        }

        fft_512();

        /* 幅度谱 |X|（Google 前端用幅度，不是功率谱） */
        for (k = 0; k < SNORE_FE_SPEC_BINS; k++)
            s_mag[k] = sqrtf(s_fft_re[k] * s_fft_re[k] + s_fft_im[k] * s_fft_im[k]);

        /* mel 滤波 + log(mel + 0.001) */
        for (b = 0; b < SNORE_MEL_BANDS; b++)
        {
            const float *w = snore_mel_matrix[b];
            float acc = 0.0f;
            for (k = 0; k < SNORE_FE_SPEC_BINS; k++)
                acc += s_mag[k] * w[k];
            out[b] = logf(acc + SNORE_LOG_OFFSET);
        }
    }
    return 0;
}
