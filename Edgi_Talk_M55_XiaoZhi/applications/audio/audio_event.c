/*
 * audio_event.c - 音频通路测试：mic0(PDM 麦克风) 能量检测与 paho MQTT 上报
 *
 * 无模型版本：以 RMS 能量超过阈值作为"音频事件"，用于验证
 * mic0 采集、检测与 MQTT 上报的完整链路。
 *
 * msh 命令：
 *   audio_event start        启动采集与事件检测
 *   audio_event stop         停止
 *   audio_event rms          打印实时电平（用于标定阈值）
 *   audio_event th <rms>     设置事件阈值
 *   audio_event pub <msg>    手动发一条 MQTT 消息测试
 *   audio_event status       查看状态
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>
#include <math.h>

#include "audio_event.h"

#ifdef PKG_USING_PAHOMQTT
#include "paho_mqtt.h"
#endif

#ifdef RT_USING_AUDIO

#define AE_MIC_DEVICE        "mic0"
#define AE_SAMPLE_RATE       16000
#define AE_CHANNELS          1
#define AE_SAMPLE_BITS       16
#define AE_CHUNK_BYTES       1024      /* 512 样本 @16kHz = 32ms */
#define AE_MIC_GAIN          30
#define AE_THREAD_STACK      4096
#define AE_THREAD_PRIO       23      /* 低于 LVGL(21)，避免采集抢占刷屏；app_data 为 24 */
#define AE_COOLDOWN_MS       3000      /* 事件冷却，防连发 */
#define AE_DEFAULT_THRESHOLD 1500      /* RMS 阈值，用 audio_event rms 标定 */

/* 请填写自己的 MQTT broker 地址与凭据，例如 "tcp://192.168.1.100:1883"。
 * 留空则本模块不启动 MQTT（音频电平检测照常运行，仅事件不上报）。 */
#define AE_MQTT_URI          ""
#define AE_MQTT_USERNAME     ""
#define AE_MQTT_PASSWORD     ""
#define AE_MQTT_TOPIC        "wfizzz/sound"

static rt_thread_t ae_thread = RT_NULL;
static volatile rt_bool_t ae_running = RT_FALSE;
static volatile rt_bool_t ae_stop_req = RT_FALSE;
static volatile rt_uint32_t ae_rms_last = 0;       /* 最近一块的 RMS */
static volatile rt_uint32_t ae_threshold = AE_DEFAULT_THRESHOLD;
static volatile rt_uint32_t ae_events = 0;         /* 事件计数 */

#ifdef PKG_USING_PAHOMQTT
static MQTTClient ae_client;
static volatile rt_bool_t ae_mqtt_started = RT_FALSE;
static char ae_client_id[24];

static void ae_mqtt_connect_cb(MQTTClient *c)  { rt_kprintf("[ae] mqtt connecting...\n"); }
static void ae_mqtt_online_cb(MQTTClient *c)   { rt_kprintf("[ae] mqtt ONLINE, broker=%s topic=%s\n", AE_MQTT_URI, AE_MQTT_TOPIC); }
static void ae_mqtt_offline_cb(MQTTClient *c)  { rt_kprintf("[ae] mqtt offline, will retry\n"); }

static int ae_mqtt_start(void)
{
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;

    if (ae_mqtt_started)
        return RT_EOK;

    if (AE_MQTT_URI[0] == '\0')
    {
        rt_kprintf("[ae] mqtt uri not configured, events will not be reported\n");
        return -RT_EINVAL;
    }

    rt_snprintf(ae_client_id, sizeof(ae_client_id), "edgi_%08x",
                (rt_uint32_t)rt_tick_get());

    rt_memset(&ae_client, 0, sizeof(ae_client));
    ae_client.uri = AE_MQTT_URI;
    rt_memcpy(&ae_client.condata, &condata, sizeof(condata));
    ae_client.condata.clientID.cstring = ae_client_id;
    ae_client.condata.keepAliveInterval = 30;
    ae_client.condata.cleansession = 1;

    /* 树莓派 mosquitto 开了 allow_anonymous false，必须带凭据 */
    ae_client.condata.username.cstring = AE_MQTT_USERNAME;
    ae_client.condata.password.cstring = AE_MQTT_PASSWORD;

    ae_client.buf_size = ae_client.readbuf_size = 1024;
    ae_client.buf = rt_calloc(1, ae_client.buf_size);
    ae_client.readbuf = rt_calloc(1, ae_client.readbuf_size);
    if (!(ae_client.buf && ae_client.readbuf))
    {
        rt_kprintf("[ae] mqtt buffer malloc failed\n");
        return -RT_ERROR;
    }

    ae_client.connect_callback = ae_mqtt_connect_cb;
    ae_client.online_callback  = ae_mqtt_online_cb;
    ae_client.offline_callback = ae_mqtt_offline_cb;

    paho_mqtt_start(&ae_client);
    ae_mqtt_started = RT_TRUE;
    rt_kprintf("[ae] mqtt thread started, clientID=%s\n", ae_client_id);
    return RT_EOK;
}

static void ae_mqtt_publish_event(rt_uint32_t rms)
{
    char json[96];

    if (!ae_client.isconnected)
    {
        rt_kprintf("[ae] event rms=%u (mqtt not connected, skip)\n", rms);
        return;
    }

    rt_snprintf(json, sizeof(json),
                "{\"event\":\"sound\",\"rms\":%u,\"count\":%u}",
                rms, ae_events);
    paho_mqtt_publish(&ae_client, QOS1, AE_MQTT_TOPIC, json);
    rt_kprintf("[ae] published: %s\n", json);
}

static int ae_mqtt_publish_text(const char *msg)
{
    if (!ae_mqtt_started)
    {
        rt_kprintf("[ae] mqtt not started (run: audio_event start)\n");
        return -RT_ERROR;
    }
    return paho_mqtt_publish(&ae_client, QOS1, AE_MQTT_TOPIC, msg);
}
#endif /* PKG_USING_PAHOMQTT */

/* 计算 16bit PCM 块的 RMS */
static rt_uint32_t ae_calc_rms(const rt_int16_t *samples, rt_size_t count)
{
    rt_int64_t acc = 0;

    for (rt_size_t i = 0; i < count; i++)
    {
        rt_int32_t v = samples[i];
        acc += (rt_int64_t)v * v;
    }
    return (rt_uint32_t)sqrt((double)acc / (double)count);
}

/* 供调试/未来模块读取实时电平与运行状态 */
rt_uint32_t audio_event_get_rms(void)
{
    return ae_rms_last;
}

rt_bool_t audio_event_is_running(void)
{
    return ae_running;
}

/* audio_event_get_status、_start、_stop、_set_threshold 定义在
 * ae_entry() 之后，因为 _start 需要引用它；使用前置声明会将该顺序问题
 * 转化为更隐蔽的隐式声明。 */

static rt_err_t ae_mic_configure(rt_device_t mic)
{
    struct rt_audio_caps caps;

    rt_memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_INPUT;
    caps.sub_type = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = AE_SAMPLE_RATE;
    caps.udata.config.channels = AE_CHANNELS;
    caps.udata.config.samplebits = AE_SAMPLE_BITS;
    rt_device_control(mic, AUDIO_CTL_CONFIGURE, &caps);

    /* 麦克风增益 */
    rt_memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_MIXER;
    caps.sub_type = AUDIO_MIXER_VOLUME;
    caps.udata.value = AE_MIC_GAIN;
    rt_device_control(mic, AUDIO_CTL_CONFIGURE, &caps);

    return RT_EOK;
}

static void ae_entry(void *parameter)
{
    rt_device_t mic;
    rt_uint8_t *buf;
    rt_tick_t last_event = 0;
    rt_size_t samples;

    RT_UNUSED(parameter);

    mic = rt_device_find(AE_MIC_DEVICE);
    if (mic == RT_NULL)
    {
        rt_kprintf("[ae] %s not found (enable BSP_USING_AUDIO + RECORD)\n",
                   AE_MIC_DEVICE);
        goto out;
    }

    buf = rt_malloc(AE_CHUNK_BYTES);
    if (buf == RT_NULL)
    {
        rt_kprintf("[ae] chunk malloc failed\n");
        goto out;
    }

    if (rt_device_open(mic, RT_DEVICE_OFLAG_RDONLY) != RT_EOK)
    {
        rt_kprintf("[ae] open %s failed\n", AE_MIC_DEVICE);
        rt_free(buf);
        goto out;
    }
    ae_mic_configure(mic);
    rt_kprintf("[ae] mic started: %uHz %uch %ubit gain=%u threshold=%u\n",
               AE_SAMPLE_RATE, AE_CHANNELS, AE_SAMPLE_BITS,
               AE_MIC_GAIN, ae_threshold);

    while (!ae_stop_req)
    {
        rt_size_t len = rt_device_read(mic, 0, buf, AE_CHUNK_BYTES);
        if (len == 0)
        {
            rt_thread_mdelay(1);
            continue;
        }

        samples = len / (AE_SAMPLE_BITS / 8);
        ae_rms_last = ae_calc_rms((rt_int16_t *)buf, samples);
        /* 旧频谱 UI 已移除，audio_spectrum_feed 暂无消费者，不再计算 FFT */

        if (ae_rms_last > ae_threshold)
        {
            rt_tick_t now = rt_tick_get();
            if (last_event == 0 ||
                (now - last_event) > rt_tick_from_millisecond(AE_COOLDOWN_MS))
            {
                last_event = now;
                ae_events++;
                rt_kprintf("[ae] EVENT #%u rms=%u (th=%u)\n",
                           ae_events, ae_rms_last, ae_threshold);
#ifdef PKG_USING_PAHOMQTT
                ae_mqtt_publish_event(ae_rms_last);
#endif
            }
        }
    }

    rt_device_close(mic);
    rt_free(buf);
    rt_kprintf("[ae] mic stopped, total events=%u\n", ae_events);

out:
    ae_running = RT_FALSE;
    ae_stop_req = RT_FALSE;
    ae_thread = RT_NULL;
}

/* ---------------- 对外接口（msh 与 UI 抽屉共用） ---------------- */

void audio_event_get_status(audio_event_status_t *st)
{
    if (st == RT_NULL)
        return;

    st->running    = ae_running;
    st->rms_last   = ae_rms_last;
    st->threshold  = ae_threshold;
    st->events     = ae_events;
#ifdef PKG_USING_PAHOMQTT
    st->mqtt_started   = ae_mqtt_started;
    st->mqtt_connected = ae_mqtt_started && ae_client.isconnected;
#else
    st->mqtt_started   = RT_FALSE;
    st->mqtt_connected = RT_FALSE;
#endif
}

int audio_event_start(void)
{
    if (ae_running)
    {
        rt_kprintf("[ae] already running\n");
        return 0;
    }
#ifdef PKG_USING_PAHOMQTT
    ae_mqtt_start();
#endif
    ae_stop_req = RT_FALSE;
    ae_running = RT_TRUE;
    ae_thread = rt_thread_create("ae", ae_entry, RT_NULL,
                                 AE_THREAD_STACK, AE_THREAD_PRIO, 10);
    if (ae_thread == RT_NULL)
    {
        ae_running = RT_FALSE;
        rt_kprintf("[ae] thread create failed\n");
        return -RT_ENOMEM;
    }
    rt_thread_startup(ae_thread);
    return 0;
}

void audio_event_stop(void)
{
    if (!ae_running)
        return;
    ae_stop_req = RT_TRUE;
    rt_kprintf("[ae] stopping...\n");
}

void audio_event_set_threshold(rt_uint32_t th)
{
    ae_threshold = th;
    rt_kprintf("[ae] threshold=%u\n", (unsigned)ae_threshold);
}

static int audio_event(int argc, char **argv)
{
    const char *cmd = "status";

    if (argc > 1)
        cmd = argv[1];

    if (rt_strcmp(cmd, "start") == 0)
    {
        return (audio_event_start() == 0) ? RT_EOK : -RT_ERROR;
    }

    if (rt_strcmp(cmd, "stop") == 0)
    {
        audio_event_stop();
        return RT_EOK;
    }

    if (rt_strcmp(cmd, "rms") == 0)
    {
        /* 打印 3 秒电平，用于标定阈值：安静时记 X，说话时记 Y，阈值取中间 */
        for (int i = 0; i < 10; i++)
        {
            rt_kprintf("rms=%u (th=%u)\n", ae_rms_last, ae_threshold);
            rt_thread_mdelay(300);
        }
        return RT_EOK;
    }

    if (rt_strcmp(cmd, "th") == 0)
    {
        if (argc < 2 + 1)
        {
            rt_kprintf("usage: audio_event th <rms>\n");
            return -RT_ERROR;
        }
        audio_event_set_threshold((rt_uint32_t)strtoul(argv[2], RT_NULL, 0));
        return RT_EOK;
    }

    if (rt_strcmp(cmd, "pub") == 0)
    {
#ifdef PKG_USING_PAHOMQTT
        const char *msg = (argc > 2) ? argv[2] : "hello from edgi_talk";
        return ae_mqtt_publish_text(msg);
#else
        rt_kprintf("[ae] PKG_USING_PAHOMQTT not enabled\n");
        return -RT_ERROR;
#endif
    }

    if (rt_strcmp(cmd, "status") == 0)
    {
        rt_kprintf("[ae] running=%s mqtt=%s events=%u rms_last=%u th=%u\n",
                   ae_running ? "yes" : "no",
#ifdef PKG_USING_PAHOMQTT
                   (ae_mqtt_started && ae_client.isconnected) ? "online" :
                   (ae_mqtt_started ? "connecting" : "off"),
#else
                   "disabled",
#endif
                   ae_events, ae_rms_last, ae_threshold);
        return RT_EOK;
    }

    rt_kprintf("usage: audio_event start|stop|rms|th <rms>|pub <msg>|status\n");
    return -RT_ERROR;
}
MSH_CMD_EXPORT(audio_event, audio event detection + mqtt report (path test));

#else /* RT_USING_AUDIO */

/* 音频未启用时的桩：app_registry 无条件引用这些符号 */
void audio_event_get_status(audio_event_status_t *st)
{
    if (st == RT_NULL)
        return;
    rt_memset(st, 0, sizeof(*st));
}

int audio_event_start(void)
{
    rt_kprintf("audio disabled: enable BSP_USING_AUDIO + BSP_USING_AUDIO_RECORD\n");
    return -RT_ENOSYS;
}

void audio_event_stop(void) { }
void audio_event_set_threshold(rt_uint32_t th) { RT_UNUSED(th); }

static int audio_event(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);
    rt_kprintf("audio disabled: enable BSP_USING_AUDIO + BSP_USING_AUDIO_RECORD\n");
    return -RT_ERROR;
}
MSH_CMD_EXPORT(audio_event, audio event detection + mqtt report (path test));

#endif /* RT_USING_AUDIO */
