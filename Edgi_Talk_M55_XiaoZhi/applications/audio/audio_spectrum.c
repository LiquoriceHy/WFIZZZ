/*
 * audio_spectrum.c - 音频频谱提取（512点FFT，零外部依赖）
 *
 * audio_event.c 的采集线程每读一块 PCM（512样本 @16kHz）就调
 * audio_spectrum_feed()：加汉宁窗 -> radix-2 FFT -> 幅度谱按对数频段
 * 分成 AE_SPECTRUM_BINS 根柱（0~8kHz），供屏幕画频谱图。
 *
 * audio_spectrum_get() 供 UI 线程拷贝当前频谱（临界区保护）。
 */

#include <rtthread.h>
#include <math.h>

#define AE_FFT_SIZE        512
#define AE_FFT_BINS        (AE_FFT_SIZE / 2)   /* 256 个有效频率点 */
#define AE_SPECTRUM_BINS   48                  /* 屏幕上的柱数 */

/* 频率分组边界（对数分布，低频分辨率高、高频跨度大，视觉更均衡）：
   柱 i 覆盖 FFT bin [spec_bound[i], spec_bound[i+1]) */
static int spec_bound[AE_SPECTRUM_BINS + 1];
static float hann_win[AE_FFT_SIZE];

static rt_uint8_t spec_bins[AE_SPECTRUM_BINS];       /* 0~255 幅度 */
static float fft_re[AE_FFT_SIZE];
static float fft_im[AE_FFT_SIZE];
static rt_int16_t accum_buf[AE_FFT_SIZE];            /* 采样积累缓冲 */
static int accum_len = 0;                            /* 已积累样本数 */
static volatile rt_bool_t spec_ready = RT_FALSE;

static void spectrum_init(void)
{
    static rt_bool_t inited = RT_FALSE;
    int i;

    if (inited)
        return;
    inited = RT_TRUE;

    for (i = 0; i < AE_FFT_SIZE; i++)
    {
        /* 汉宁窗，抑制频谱泄漏 */
        hann_win[i] = 0.5f * (1.0f - cosf(2.0f * 3.14159265f * i / (AE_FFT_SIZE - 1)));
    }

    /* 对数频段边界：0 .. 256 */
    for (i = 0; i <= AE_SPECTRUM_BINS; i++)
    {
        double f = (double)AE_FFT_BINS * pow(2.0, 8.0 * i / AE_SPECTRUM_BINS) / 256.0;
        int b = (int)(f * AE_FFT_BINS / 256.0);
        spec_bound[i] = (b > AE_FFT_BINS) ? AE_FFT_BINS : b;
        if (i > 0 && spec_bound[i] <= spec_bound[i - 1])
            spec_bound[i] = spec_bound[i - 1] + 1;
    }
    spec_bound[AE_SPECTRUM_BINS] = AE_FFT_BINS;
}

/* 迭代式 radix-2 FFT（float，原地） */
static void fft_512(float *re, float *im)
{
    const int n = AE_FFT_SIZE;
    int i, j, len;

    for (i = 1, j = 0; i < n; i++)              /* 位反转重排 */
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
        {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }

    for (len = 2; len <= n; len <<= 1)          /* 蝶形运算 */
    {
        float ang = -2.0f * 3.14159265f / len;
        float wr = cosf(ang), wi = sinf(ang);
        for (i = 0; i < n; i += len)
        {
            float cwr = 1.0f, cwi = 0.0f;
            for (j = 0; j < len / 2; j++)
            {
                int a = i + j, b = i + j + len / 2;
                float tr = re[b] * cwr - im[b] * cwi;
                float ti = re[b] * cwi + im[b] * cwr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                {
                    float nwr = cwr * wr - cwi * wi;
                    cwi = cwr * wi + cwi * wr;
                    cwr = nwr;
                }
            }
        }
    }
}

/* 对积累缓冲做一次 FFT + 频段分组 */
static void spectrum_run(void)
{
    int i;

    for (i = 0; i < AE_FFT_SIZE; i++)
    {
        fft_re[i] = accum_buf[i] * hann_win[i];
        fft_im[i] = 0.0f;
    }

    fft_512(fft_re, fft_im);

    rt_enter_critical();
    for (i = 0; i < AE_SPECTRUM_BINS; i++)
    {
        float mag = 0.0f;
        int b, lo = spec_bound[i], hi = spec_bound[i + 1];

        for (b = lo; b < hi; b++)   /* 频段内取峰值 */
        {
            float m = sqrtf(fft_re[b] * fft_re[b] + fft_im[b] * fft_im[b]);
            if (m > mag)
                mag = m;
        }
        mag /= (AE_FFT_SIZE / 4.0f);                    /* 归一化 */
        {
            float db = 20.0f * log10f(mag + 1e-6f);     /* 对数显示更符合听感 */
            int v = (int)((db + 60.0f) / 60.0f * 255.0f);
            spec_bins[i] = (rt_uint8_t)((v < 0) ? 0 : ((v > 255) ? 255 : v));
        }
    }
    spec_ready = RT_TRUE;
    rt_exit_critical();
}

/* 采集线程喂 PCM：碎块先积累，攒满一个 FFT 窗口（512样本）再运算 */
void audio_spectrum_feed(const rt_int16_t *samples, int count)
{
    static rt_bool_t first_call = RT_TRUE;

    if (first_call)
    {
        first_call = RT_FALSE;
        rt_kprintf("[spec] feed first call, count=%d\n", count);
    }

    if (count <= 0 || samples == RT_NULL)
        return;
    spectrum_init();

    while (count > 0)
    {
        int need = AE_FFT_SIZE - accum_len;
        int copy = (count < need) ? count : need;

        rt_memcpy(&accum_buf[accum_len], samples,
                  (rt_size_t)copy * sizeof(rt_int16_t));
        accum_len += copy;
        samples += copy;
        count -= copy;

        if (accum_len >= AE_FFT_SIZE)
        {
            spectrum_run();
            accum_len = 0;      /* 不重叠：每 32ms 一帧频谱 */
        }
    }
}

/* UI 线程拷贝当前频谱，n 应为 AE_SPECTRUM_BINS */
int audio_spectrum_get(rt_uint8_t *out, int n)
{
    if (n > AE_SPECTRUM_BINS)
        n = AE_SPECTRUM_BINS;
    rt_enter_critical();
    rt_memcpy(out, spec_bins, n);
    rt_exit_critical();
    return spec_ready ? n : 0;
}
