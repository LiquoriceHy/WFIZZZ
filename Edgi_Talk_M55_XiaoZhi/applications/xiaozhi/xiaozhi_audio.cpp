/*
 * Copyright (c) 2006-2025, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2025-12-14     RT-Thread    First version
 */
#include <rtdevice.h>
#include <rtthread.h>
#include "board.h"
#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"
#include "xiaozhi.h"

/* Device name configurations */
#ifndef BSP_XIAOZHI_SOUND_DEVICE_NAME
#define BSP_XIAOZHI_SOUND_DEVICE_NAME "sound0"
#endif

#ifndef BSP_XIAOZHI_MIC_DEVICE_NAME
#define BSP_XIAOZHI_MIC_DEVICE_NAME "mic0"
#endif

/* Include opus heap functions */
#ifdef __cplusplus
extern "C"
{
#endif
    extern void *opus_heap_malloc(uint32_t size);
    extern void opus_heap_free(void *p);
#ifdef __cplusplus
}
#endif

#define DBG_TAG "xz.audio"
#define DBG_LVL DBG_WARNING  /* Reduce log level to save memory */
#include <rtdbg.h>

/* Global Variables */
static xz_audio_t xz_audio;
static rt_event_t mic_event = RT_NULL;

void mic_thread_entry(void *param);
void xz_opus_thread_entry(void *p);

/* Audio Initialization */
void xz_sound_init(void)
{
    /* TTS 播放已按需求关闭（见 xiaozhi.cpp 的 WS_DATA 分支），因此此处
     * 不再打开扬声器设备：打开一个永不写入的输出流只会占用 ES8388
     * 的 I2S 通道与 DMA 缓冲，对录音链路没有益处。
     *
     * 保留 rt_device_find 与 CONFIGURE：音量控制走 MCP 的
     * self.audio_speaker.set_volume（es8388_volume_set），该路径需要设备
     * 已被上层注册，与是否 open 无关。
     *
     * 如需恢复 TTS 播放：将下面两行 rt_device_open 恢复。 */
    xz_audio_t *thiz = &xz_audio;
    thiz->rt_audio_dev = rt_device_find(BSP_XIAOZHI_SOUND_DEVICE_NAME);
    struct rt_audio_caps sound_dev_arg;
    sound_dev_arg.main_type = AUDIO_TYPE_OUTPUT;
    sound_dev_arg.sub_type = AUDIO_DSP_PARAM;
    sound_dev_arg.udata.value = 30;
    struct rt_audio_configure audio_configure;
    audio_configure.channels = 1;
    audio_configure.samplebits = 16;
    audio_configure.samplerate = 16000;
    sound_dev_arg.udata.config = audio_configure;
    rt_device_control(thiz->rt_audio_dev, AUDIO_CTL_CONFIGURE, &sound_dev_arg);

    LOG_I("TTS playback disabled: speaker device not opened");
}

int xz_mic_init(void)
{
    xz_audio_t *thiz = &xz_audio;
    thiz->rt_mic_dev = rt_device_find(BSP_XIAOZHI_MIC_DEVICE_NAME);
    thiz->is_rx_enable = 0;  /* Initialize as disabled */

    struct rt_audio_caps mic_dev_arg;
    mic_dev_arg.main_type = AUDIO_TYPE_MIXER;
    mic_dev_arg.sub_type = AUDIO_MIXER_VOLUME;
    mic_dev_arg.udata.value = 30;
    rt_device_control(thiz->rt_mic_dev, AUDIO_CTL_CONFIGURE, &mic_dev_arg);

    mic_event = rt_event_create("mic_evt", RT_IPC_FLAG_FIFO);
    RT_ASSERT(mic_event != RT_NULL);
    LOG_I("xz_mic_init");

    rt_thread_t tid = rt_thread_create("mic_thread", mic_thread_entry,
                                       RT_NULL, 1024 * 2, 6, 10);
    RT_ASSERT(tid != RT_NULL);
    rt_thread_startup(tid);

    return RT_EOK;
}

/* Microphone Functions */
void xz_mic_open(xz_audio_t *thiz)
{
    if (!thiz->is_rx_enable)
    {
        if (rt_device_open(thiz->rt_mic_dev, RT_DEVICE_OFLAG_RDONLY) == RT_EOK)
        {
            rt_event_send(mic_event, MIC_EVENT_OPEN);
            LOG_I("Audio: Microphone opened successfully");
            thiz->is_rx_enable = 1;
        }
        else
        {
            LOG_E("Audio: Failed to open microphone device");
        }
    }
    else
    {
        LOG_D("Microphone already enabled");
    }
}

void xz_mic_close(xz_audio_t *thiz)
{
    if (thiz->is_rx_enable)
    {
        rt_event_send(mic_event, MIC_EVENT_CLOSE);
        rt_device_close(thiz->rt_mic_dev);
        LOG_I("Audio: Microphone closed");
        thiz->is_rx_enable = 0;
    }
    else
    {
        LOG_D("Audio: Microphone already disabled");
    }
}

void xz_mic(int on)
{
    xz_audio_t *thiz = &xz_audio;
    if (on)
    {
        xz_mic_open(thiz);
    }
    else
    {
        xz_mic_close(thiz);
    }
}

/* Get microphone enable status */
int xz_mic_is_enabled(void)
{
    return xz_audio.is_rx_enable ? 1 : 0;
}

/* Speaker Functions */
void xz_speaker_open(xz_audio_t *thiz)
{
    /* TTS 播放已关闭：不置 is_tx_enable。
     *
     * xiaozhi.cpp 的 tts start 分支仍会调用 xz_speaker(1)（该调用属于状态机
     * 的一部分，不影响文字回复），但此处保持 0，opus 线程的 DOWNLINK
     * 分支就不会解码 Opus，也不会对未打开的扬声器设备调用 rt_device_write。
     *
     * 双保险：WS_DATA 帧已在 xiaozhi.cpp 侧丢弃，XZ_EVENT_DOWNLINK 不存在
     * 生产者。但状态机日后可能改动，此处再作一层防护。 */
    RT_UNUSED(thiz);
}

void xz_speaker_close(xz_audio_t *thiz)
{
    LOG_D("speaker off");
    LOG_D("\u5f85\u547d\u4e2d...");
    thiz->is_tx_enable = 0;
    rt_slist_t *idle;
    rt_enter_critical();
    while (1)
    {
        idle = rt_slist_first(&thiz->downlink_decode_busy);
        if (idle)
        {
            rt_slist_remove(&thiz->downlink_decode_busy, idle);
            rt_slist_append(&thiz->downlink_decode_idle, idle);
        }
        else
        {
            break;
        }
    }
    rt_exit_critical();
}

void xz_speaker(int on)
{
    xz_audio_t *thiz = &xz_audio;
    if (on)
    {
        xz_speaker_open(thiz);
    }
    else
    {
        xz_speaker_close(thiz);
    }
}

/* Audio Processing */

/* 麦克风 RMS：供 UI 作图形反馈。
 *
 * 此处只计算原始 RMS，不做任何映射，因为 UI 侧已有 rms_to_level() 的对数
 * 映射（app_registry.c），且那边已针对鼾声检测调好斜率。在音频线程中再
 * 叠加一层线性映射，只会造成两个界面手感不一致。
 *
 * 代价：mic 线程本就遍历这 1024 字节（原代码直接写入 ringbuffer），
 * 多一次 int16 遍历可以忽略，无需额外线程或定时器。 */
static volatile rt_uint32_t s_mic_rms = 0;

extern "C" rt_uint32_t xz_mic_rms_get(void)
{
    return s_mic_rms;
}

static void mic_rms_update(const uint8_t *buf, int len)
{
    const rt_int16_t *s = (const rt_int16_t *)buf;
    int n = len / 2;
    rt_int64_t sum = 0;
    int i;
    unsigned long long acc, r, bit;

    if (n <= 0)
        return;

    for (i = 0; i < n; i++)
    {
        rt_int32_t v = s[i];

        sum += (rt_int64_t)v * v;
    }

    /* 整数开方。累加用 int64 兜住：1024 字节 = 512 个 int16，
     * 每个最大 32767^2 ≈ 1.07e9，全满时平方和 ≈ 5.5e11，超出 int32。 */
    acc = (unsigned long long)sum / (unsigned)n;
    r = 0;
    bit = 1ULL << 30;
    while (bit > acc) bit >>= 2;
    while (bit)
    {
        if (acc >= r + bit)
        {
            acc -= r + bit;
            r = (r >> 1) + bit;
        }
        else
        {
            r >>= 1;
        }
        bit >>= 2;
    }

    /* 一阶平滑，系数 3/4 旧值：不平滑会随音节乱抖，过度平滑跟不上开合 */
    s_mic_rms = (s_mic_rms * 3 + (rt_uint32_t)r) / 4;
}

void mic_thread_entry(void *param)
{
    rt_uint32_t evt = 0;
    uint8_t buffer[1024] = {0};
    int length = 0, total_length = 0;

    while (1)
    {
        if (rt_event_recv(mic_event, MIC_EVENT_OPEN,
                          RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                          RT_WAITING_FOREVER, &evt) == RT_EOK)
        {
            while (1)
            {
                rt_event_recv(mic_event, MIC_EVENT_CLOSE,
                              RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                              RT_WAITING_NO, &evt);
                if (evt & MIC_EVENT_CLOSE)
                {
                    break;
                }
                length = rt_device_read(xz_audio.rt_mic_dev, 0, buffer, 1024);
                if (length > 0)
                {
                    mic_rms_update(buffer, length);

                    total_length += length;
                    rt_ringbuffer_put(xz_audio.rb_opus_encode_input, (uint8_t *)buffer, length);
                }
                else
                {
                    /* 没有新数据时让电平自然回落，否则录音停止后条子会
                     * 冻在最后一帧的值上 */
                    s_mic_rms = (s_mic_rms * 3) / 4;
                }
                if (total_length >= XZ_MIC_FRAME_LEN)
                {
                    rt_event_send(xz_audio.event, XZ_EVENT_MIC_RX);
                    total_length = 0;
                }
            }
        }
    }
}

void xz_opus_thread_entry(void *p)
{
    int err;
    xz_audio_t *thiz = &xz_audio;
    thiz->decoder = opus_decoder_create(24000, 1, &err);
    RT_ASSERT(thiz->decoder);
    /* 编码器必须是单声道。
     *
     * 原代码传的是 2（立体声），但整条链路只有一路单声道数据：
     *   - hello 声明 "channels":1（xiaozhi.h 的 HELLO_MESSAGE）
     *   - mic 回调每次 put 的长度就是 XZ_MIC_FRAME_LEN 的单声道字节数
     *   - opus_encode 传的 frame_size 是 XZ_MIC_FRAME_LEN/2（=960 采样）
     * 传 2 时 Opus 认为每帧为 960 个立体声帧，但实际只喂入 960 个 int16
     * 单声道样本，后半段读到的是上一帧残留；且编码器内部多维护一套声道
     * 状态，额外消耗栈空间，与 24KB 栈叠加即为 "thread:opus stack overflow"
     * 的成因之一。 */
    thiz->encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &err);
    RT_ASSERT(thiz->encoder);

    opus_encoder_ctl(thiz->encoder, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_60_MS));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_VBR_CONSTRAINT(1));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_BITRATE(16000));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_LSB_DEPTH(16));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_DTX(0));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_INBAND_FEC(0));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_PACKET_LOSS_PERC(0));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_PREDICTION_DISABLED(0));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
    opus_encoder_ctl(thiz->encoder, OPUS_SET_BANDWIDTH(OPUS_AUTO));

    while (!thiz->is_exit)
    {
        rt_uint32_t evt = 0;
        rt_event_recv(thiz->event, XZ_EVENT_ALL,
                      RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                      RT_WAITING_FOREVER, &evt);

        if ((evt & XZ_EVENT_MIC_RX) && thiz->is_rx_enable)
        {
            rt_ringbuffer_get(thiz->rb_opus_encode_input, (uint8_t *)&thiz->encode_in[0], XZ_MIC_FRAME_LEN);
            opus_int32 len = opus_encode(thiz->encoder, (const opus_int16 *)thiz->encode_in,
                                         XZ_MIC_FRAME_LEN / 2, thiz->encode_out, XZ_MIC_FRAME_LEN);
            if (len < 0 || len > XZ_MIC_FRAME_LEN)
            {
                LOG_E("opus_encode len=%d", len);
                RT_ASSERT(0);
            }
            xz_audio_send_using_websocket(thiz->encode_out, len);
        }

        if ((evt & XZ_EVENT_DOWNLINK) && thiz->is_tx_enable)
        {
            rt_slist_t *decode;
            rt_enter_critical();
            decode = rt_slist_first(&thiz->downlink_decode_busy);
            rt_exit_critical();
            xz_decode_queue_t *queue = rt_container_of(decode, xz_decode_queue_t, node);
            opus_int32 res = opus_decode(thiz->decoder, (const uint8_t *)queue->data,
                                         queue->data_len, thiz->downlink_decode_out,
                                         XZ_SPK_FRAME_LEN / 2, 0);
            if (res < 0)
            {
                LOG_E("Opus decode error: %d", res);
            }
            rt_device_write(thiz->rt_audio_dev, 0, (char *)thiz->downlink_decode_out, XZ_SPK_FRAME_LEN);

            uint8_t need_decode_again = 0;
            rt_enter_critical();
            rt_slist_remove(&thiz->downlink_decode_busy, decode);
            rt_slist_append(&thiz->downlink_decode_idle, decode);
            if (rt_slist_first(&thiz->downlink_decode_busy))
            {
                need_decode_again = true;
            }
            rt_exit_critical();

            if (need_decode_again)
                rt_event_send(thiz->event, XZ_EVENT_DOWNLINK);
        }
    }

    if (thiz->encoder)
        opus_encoder_destroy(thiz->encoder);
    if (thiz->decoder)
        opus_decoder_destroy(thiz->decoder);

    LOG_D("---xz thread exit---");
}

void xz_audio_decoder_encoder_open(uint8_t is_websocket)
{
    xz_audio_t *thiz = &xz_audio;
    if (!thiz->inited)
    {
        thiz->is_websocket = is_websocket;
        thiz->event = rt_event_create("xzaudio", RT_IPC_FLAG_FIFO);
        RT_ASSERT(thiz->event);
        rt_slist_init(&thiz->downlink_decode_idle);
        rt_slist_init(&thiz->downlink_decode_busy);
        for (int i = 0; i < XZ_DOWNLINK_QUEUE_NUM; i++)
        {
            thiz->downlink_queue[i].size = 256;
            thiz->downlink_queue[i].data =
                (uint8_t *)opus_heap_malloc(thiz->downlink_queue[i].size);
            RT_ASSERT(thiz->downlink_queue[i].data);

            // 将新分配的队列节点添加到空闲队列中
            rt_slist_append(&thiz->downlink_decode_idle,
                            &thiz->downlink_queue[i].node);
        }

        thiz->rb_opus_encode_input = rt_ringbuffer_create(XZ_MIC_FRAME_LEN * 2);
        RT_ASSERT(thiz->rb_opus_encode_input);

        /* 栈 40KB。
         *
         * 曾依据"主堆只有约 90KB"降至 24KB，但该数字有误。实测主堆从
         * __end__(0x2613CD68) 到 __HeapLimit(0x261C0000) 共 520KB，真实
         * 瓶颈是 HyperRAM 而非主堆。24KB 运行 opus_encode 会在真机上溢出：
         *     thread:opus stack overflow
         * 一帧 60ms(960 采样) 的编码峰值栈约为 30-35KB，40KB 留有余量。
         * 日后如需压缩，先以 list_thread 查看 opus 线程实际水位，不要按估算削减。 */
        thiz->thread = rt_thread_create("opus", xz_opus_thread_entry, NULL,
                                        1024 * 40, 8, 10);
        RT_ASSERT(thiz->thread != RT_NULL);
        rt_thread_startup(thiz->thread);

        thiz->inited = 1;
    }
}

void xz_audio_decoder_encoder_close(void)
{
    xz_audio_t *thiz = &xz_audio;
    thiz->is_exit = 1;
    while (rt_thread_find("opus"))
    {
        LOG_D("wait thread opus exit");
        rt_thread_mdelay(100);
    }

    rt_ringbuffer_destroy(thiz->rb_opus_encode_input);
    rt_event_delete(thiz->event);

    for (int i = 0; i < XZ_DOWNLINK_QUEUE_NUM; i++)
    {
        if (thiz->downlink_queue[i].data)
        {
            opus_heap_free(thiz->downlink_queue[i].data);
            thiz->downlink_queue[i].data = NULL;
        }
    }
}

/* Downlink Handling */
void xz_audio_downlink(uint8_t *data, uint32_t size, uint32_t *aes_value, uint8_t need_aes)
{
    xz_audio_t *thiz = &xz_audio;
    rt_slist_t *idle;
    int try_times = 0;

    if (!thiz->inited)
        return;

wait_speaker:
    rt_enter_critical();
    idle = rt_slist_first(&thiz->downlink_decode_idle);
    if (!idle)
    {
        rt_exit_critical();
        LOG_I("speaker busy");
        try_times++;
        if (try_times < 20)
        {
            rt_thread_mdelay(5);
            goto wait_speaker;
        }
        return;
    }
    rt_slist_remove(&thiz->downlink_decode_idle, idle);
    rt_exit_critical();

    xz_decode_queue_t *queue = rt_container_of(idle, xz_decode_queue_t, node);
    if (queue->size < size + 16)
    {
        if (queue->data)
            opus_heap_free(queue->data);
        queue->size = size + 16;
        queue->data = (uint8_t *)opus_heap_malloc(queue->size);
        if (queue->data == RT_NULL)
        {
            LOG_E("malloc failed");
            rt_enter_critical();
            rt_slist_append(&thiz->downlink_decode_idle, idle);
            rt_exit_critical();
            return;
        }
    }
    queue->data_len = size;
    rt_memcpy(queue->data, data, size);

    rt_enter_critical();
    rt_slist_append(&thiz->downlink_decode_busy, idle);
    rt_exit_critical();
    rt_event_send(thiz->event, XZ_EVENT_DOWNLINK);
}
