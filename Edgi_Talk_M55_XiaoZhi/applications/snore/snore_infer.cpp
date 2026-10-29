/*
 * snore_infer.cpp - 鼾声推理后端（Infineon mtb_ml + Ethos-U55）
 *
 * 模型：chayuto/yamnet-mel-int8-tflm 重校准版经 Vela 4.3.0 编译
 * （ethos-u55-128，100% 算子映射到 NPU），mel patch 输入 [1,96,64] int8，
 * 输出 [1,521] int8 sigmoid 概率，Snoring = class 38。
 *
 * 推理后端使用 Infineon 官方 mtb_ml 中间件（TFLM 加 Ethos-U 驱动），
 * 而非自行装配 MicroInterpreter。原因见下。
 *
 * 改用 mtb_ml 的原因
 * ------------------
 * 自行装配 TFLM 时，Vela 的 _scratch 与 _scratch_fast 张量没有生产者，
 * 也不在 subgraph inputs 中，TFLM 的内存规划器不会为它们标记生命周期
 * （micro_allocation_info.cpp 注释：not part of the memory plan），于是
 * 它们被视为可复用内存，与输入张量分配到同一地址。对 CPU 推理没有影响，
 * 但 NPU 要求这几块缓冲在整条命令流执行期间同时有效，实测表现为：
 * 命令流被读完（QREAD == QSIZE），却永不完成、不置任何故障位、不产生
 * 中断。mtb_ml 内部对 Vela 模型有专门的 tensor arena 布局处理，
 * 使用它可避免此类手工装配引发的问题。
 *
 * 内存布局
 * --------
 * tensor arena 必须位于 NPU 可直接寻址的片上内存，此处使用链接脚本中的
 * .cy_socmem_data 段（m55_data_secondary，SOCMEM）。
 *
 * 模型本身（3.37 MB）远超 SOCMEM 容量（2 MB），只能保留在 NOR flash。
 * Vela 的 const_mem_area 允许权重存放于外部，NPU 通过 AXI 读取 NOR
 * 属于硬件支持的路径（命令流本身位于 NOR 中亦可被读取）。
 *
 * 单线程使用，不设锁。
 */

#include <rtthread.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "snore_infer.h"
#include "snore_frontend.h"

#include "mtb_ml.h"
#include "mtb_ml_model.h"
#include "cy_result.h"

/* 模型数组：普通 const，落 NOR（XIP） */
#include "snore_model_npu.h"

/* arena 大小照官方 NPU 部署例程取 891904 B（model.c 里同一份 Vela 模型的
 * 参数）。这是 TFLM 给该模型规划出的量，比凭空留余量更可靠。 */
#define SNORE_ARENA_BYTES   891904

/* arena 必须落在 NPU 可寻址的片上内存。.cy_socmem_data 由链接脚本映射到
 * m55_data_secondary（SOCMEM 0x26060000 起），启动时从 flash 拷贝。 */
static uint8_t s_arena[SNORE_ARENA_BYTES]
    __attribute__((section(".cy_socmem_data"), aligned(16)));

static mtb_ml_model_t *s_model = RT_NULL;
static MTB_ML_DATA_T *s_in  = RT_NULL;
static MTB_ML_DATA_T *s_out = RT_NULL;
static int   s_out_count = 0;
static int   s_in_zp     = 0;
static float s_in_scale  = 1.0f;
static int   s_out_zp    = 0;
static float s_out_scale = 1.0f;
static snore_info_t s_info;

/* 24KB 的 mel patch 缓冲放堆上（内部 SRAM .bss 只有 256KB，勿再添大静态量） */
static float *s_patch = RT_NULL;

static const mtb_ml_model_bin_t s_bin = {
    .name       = "snore_yamnet",
    .model_bin  = (uint8_t *)snore_model_npu,
    .model_size = SNORE_MODEL_NPU_SIZE,
    .arena_size = SNORE_ARENA_BYTES,
};

static const mtb_ml_model_buffer_t s_buffer = {
    .tensor_arena      = s_arena,
    .tensor_arena_size = SNORE_ARENA_BYTES,
};

extern "C" int snore_infer_init(void)
{
    cy_rslt_t res;
    size_t size = 0;
    int *dims = RT_NULL;
    int dim_len = 0;

    if (s_model != RT_NULL)
        return -4;                      /* 已初始化 */

    memset(&s_info, 0, sizeof(s_info));

    res = mtb_ml_init(5);               /* 优先级 5，与官方例程一致 */
    if (res != MTB_ML_RESULT_SUCCESS)
    {
        rt_kprintf("[snore] mtb_ml_init failed: 0x%08lx\n", (unsigned long)res);
        return -5;
    }

    res = mtb_ml_model_init(&s_bin, &s_buffer, &s_model);
    if (res != MTB_ML_RESULT_SUCCESS)
    {
        rt_kprintf("[snore] mtb_ml_model_init failed: 0x%08lx\n",
                   (unsigned long)res);
        return -2;
    }

    /* 输入张量：拿指针、量化参数 */
    s_in = RT_NULL;
    res = mtb_ml_model_get_input_detail(s_model, 0, &s_in, &size, &dims,
                                        &dim_len, &s_in_zp, &s_in_scale);
    if (res != MTB_ML_RESULT_SUCCESS || s_in == RT_NULL)
    {
        rt_kprintf("[snore] get_input_detail failed: 0x%08lx\n",
                   (unsigned long)res);
        return -2;
    }
    rt_kprintf("[snore] in  %u bytes, %d dims, scale=%.5f zp=%d @%p\n",
               (unsigned)size, dim_len, (double)s_in_scale, s_in_zp,
               (void *)s_in);

    if (size != (size_t)(SNORE_MEL_FRAMES * SNORE_MEL_BANDS))
    {
        rt_kprintf("[snore] unexpected input size %u (want %u)\n",
                   (unsigned)size,
                   (unsigned)(SNORE_MEL_FRAMES * SNORE_MEL_BANDS));
        return -3;
    }

    /* 输出张量 */
    s_out = RT_NULL;
    res = mtb_ml_model_get_output_detail(s_model, 0, &s_out, &size, &dims,
                                         &dim_len, &s_out_zp, &s_out_scale);
    if (res != MTB_ML_RESULT_SUCCESS || s_out == RT_NULL)
    {
        rt_kprintf("[snore] get_output_detail failed: 0x%08lx\n",
                   (unsigned long)res);
        return -2;
    }
    s_out_count = (int)size;
    rt_kprintf("[snore] out %u classes, scale=%.5f zp=%d @%p\n",
               (unsigned)size, (double)s_out_scale, s_out_zp, (void *)s_out);

    if (s_out_count < 521)
    {
        rt_kprintf("[snore] unexpected output size %d (want >=521)\n",
                   s_out_count);
        return -3;
    }

    s_patch = (float *)rt_malloc(sizeof(float) * SNORE_MEL_FRAMES * SNORE_MEL_BANDS);
    if (s_patch == RT_NULL)
    {
        rt_kprintf("[snore] patch buffer alloc failed\n");
        return -1;
    }

    s_info.arena_size = SNORE_ARENA_BYTES;
    s_info.arena_addr = s_arena;
    s_info.in_scale   = s_in_scale;
    s_info.in_zp      = s_in_zp;
    s_info.out_scale  = s_out_scale;
    s_info.out_zp     = s_out_zp;
    s_info.ok         = 1;

    rt_kprintf("[snore] ready, arena %uKB @0x%08x (SOCMEM)\n",
               (unsigned)(SNORE_ARENA_BYTES / 1024),
               (unsigned)(uintptr_t)s_arena);
    return 0;
}

extern "C" int snore_infer_window(const rt_int16_t *pcm, snore_result_t *out_res)
{
    rt_tick_t t0, t1, t2;
    int i;

    if (s_model == RT_NULL)
        return -1;
    if (pcm == RT_NULL || out_res == RT_NULL)
        return -2;

    t0 = rt_tick_get();
    snore_frontend_window(pcm, s_patch);

    /* mel patch（float）→ 模型要求的 int8。量化参数由模型自己给出。 */
    for (i = 0; i < SNORE_MEL_FRAMES * SNORE_MEL_BANDS; i++)
    {
        int q = (int)lrintf(s_patch[i] / s_in_scale) + s_in_zp;
        if (q < -128) q = -128;
        if (q > 127)  q = 127;
        s_in[i] = (MTB_ML_DATA_T)q;
    }

    t1 = rt_tick_get();

    if (mtb_ml_model_run(s_model, s_in) != MTB_ML_RESULT_SUCCESS)
    {
        rt_kprintf("[snore] mtb_ml_model_run failed\n");
        return -3;
    }

    t2 = rt_tick_get();

    /* top-5 与 Snoring 概率 */
    memset(out_res, 0, sizeof(*out_res));
    for (i = 0; i < s_out_count; i++)
    {
        float p = ((float)s_out[i] - (float)s_out_zp) * s_out_scale;

        if (p <= out_res->top_score[SNORE_TOPN - 1])
            continue;

        {
            int j = SNORE_TOPN - 1;
            while (j > 0 && out_res->top_score[j - 1] < p)
            {
                out_res->top_score[j] = out_res->top_score[j - 1];
                out_res->top_idx[j]   = out_res->top_idx[j - 1];
                j--;
            }
            out_res->top_score[j] = p;
            out_res->top_idx[j]   = (rt_uint16_t)i;
        }
    }

    out_res->snore =
        ((float)s_out[SNORE_CLASS_SNORE] - (float)s_out_zp) * s_out_scale;

    out_res->fe_ms    = (rt_uint32_t)(t1 - t0) * 1000u / RT_TICK_PER_SECOND;
    out_res->infer_ms = (rt_uint32_t)(t2 - t1) * 1000u / RT_TICK_PER_SECOND;
    return 0;
}

extern "C" void snore_infer_get_info(snore_info_t *info)
{
    if (info != RT_NULL)
        *info = s_info;
}
