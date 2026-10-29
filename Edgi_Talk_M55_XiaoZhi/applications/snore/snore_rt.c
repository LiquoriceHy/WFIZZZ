/*
 * snore_rt.c - 鼾声实时检测（mic0 采集，YAMNet 推理，MQTT 上报）
 *
 * 采用隔窗检测而非逐窗实时：单窗音频时长为 0.975s，而推理开销（mel 前端
 * 加 NPU 推理）已接近该窗口长度。逐窗实时要求每窗处理时间不超过 0.975s，
 * 当前实现（mtb_ml 加 Ethos-U55 NPU）实测推理 86~94ms、前端 241ms、
 * 合计约 337ms，相对 975ms 有约 2.9 倍余量。尽管如此仍采用检一窗跳半窗
 * 的方式，使处理节奏与采集节奏解耦；鼾声是持续数秒的节律事件，
 * 覆盖率足够。
 *
 * 重要接口事实（读源码确认）：
 *   snore_infer_window() 内部已调用 snore_frontend_window() 完成 mel 前端，
 *   调用方不得再次计算，否则将额外增加一次前端开销。
 *
 * 阈值与去抖：
 *   连续 SNORE_RT_CONSEC 窗超阈值才算一次事件（防止单窗误报）；
 *   事件后进行 SNORE_RT_COOLDOWN_MS 冷却，避免打鼾期间重复上报。
 *
 * msh 命令：
 *   snore_rt start            启动检测（自动连 MQTT）
 *   snore_rt stop             停止
 *   snore_rt status           查看状态
 *   snore_rt th <0-100>       设置阈值（百分比，60 = 0.60）
 *   snore_rt pub              手动发一条测试事件（验证 MQTT 链路）
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "snore_infer.h"
#include "snore_rt.h"
#include "mic_arb.h"
#include "app_data.h"
#include "snore_log.h"      /* 事件按天归档到 SD */

#ifdef PKG_USING_PAHOMQTT
#include "paho_mqtt.h"
#endif

#define DBG_TAG "snore.rt"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define SNORE_RT_MIC          "mic0"
#define SNORE_RT_SRATE        16000
#define SNORE_RT_GAIN         30
#define SNORE_RT_STACK        (8 * 1024)
/* 2026-10-26 真机 list_thread 实测：原 24KB 栈峰值仅用 720 字节（3%）。
 * 线程内最大局部量为 json[128]，大缓冲（s_win、chunk）均在堆上。
 * 降至 8KB 仍保留约 11 倍余量，可覆盖 NPU 推理的深层调用链。
 * 若日后在栈上增加大数组，先运行 list_thread 复核水位再调整。 */
#define SNORE_RT_PRIO         24            /* 低于 UI(21)/音频(20)/app_data(24+) */
#define SNORE_RT_CHUNK        1024          /* 512 样本 @16kHz = 32ms */
#define SNORE_RT_HOP          (SNORE_WIN_SAMPLES / 2)   /* 检一窗跳半窗 */
#define SNORE_RT_CONSEC       2
#define SNORE_RT_COOLDOWN_MS  15000

/* 请填写自己的 MQTT broker 地址与凭据，例如 "tcp://192.168.1.100:1883"。
 * 留空则本模块不启动 MQTT（鼾声检测照常运行，仅事件不上报）。 */
#define SNORE_RT_MQTT_URI     ""
#define SNORE_RT_MQTT_USER    ""
#define SNORE_RT_MQTT_PASS    ""
#define SNORE_RT_TOPIC        "wfizzz/snore"

static rt_thread_t         s_thread = RT_NULL;
static volatile rt_bool_t  s_running = RT_FALSE;
static volatile rt_bool_t  s_stop = RT_FALSE;
static volatile rt_uint16_t s_threshold = 60;     /* 0-100 = 概率 0.00-1.00 */
static volatile rt_uint32_t s_windows = 0;
static volatile rt_uint32_t s_events = 0;
static volatile rt_uint16_t s_last_prob = 0;      /* 0-100 */
static volatile rt_uint32_t s_rms = 0;            /* 最近一块 PCM 的 RMS 电平 */

/* 事件历史：抽屉详情页显示最近若干次判定。环形缓冲，满了覆盖最旧一条。
 * 96 字节静态量，不占堆；UPTIME 是事件发生时的开机秒数。 */
#define SNORE_RT_HIST_MAX   8

typedef struct
{
    rt_uint32_t uptime_s;                         /* 开机秒数，0 表示空槽 */
    rt_uint16_t prob;                             /* 0-100 */
    rt_uint16_t cost_ms;
} snore_hist_t;

static snore_hist_t s_hist[SNORE_RT_HIST_MAX];
static rt_uint32_t  s_hist_head;                  /* 下一条写入位置 */
static rt_uint32_t  s_hist_count;                 /* 已记录条数，<= MAX */

/* 大缓冲走主堆：内部 SRAM .bss 仅 256KB，静态数组会把链接撑爆 */
static rt_int16_t *s_win = RT_NULL;               /* 15600 样本 ≈ 31KB */

#ifdef PKG_USING_PAHOMQTT
static MQTTClient         s_client;
static volatile rt_bool_t s_mqtt_up = RT_FALSE;
static char               s_client_id[28];

static void snore_mqtt_online(MQTTClient *c)
{
    s_mqtt_up = RT_TRUE;
    app_data_set_mqtt(RT_TRUE);        /* 状态栏/系统页的 MQTT 指示 */
    LOG_I("mqtt online -> %s", SNORE_RT_TOPIC);
    RT_UNUSED(c);
}

static void snore_mqtt_offline(MQTTClient *c)
{
    s_mqtt_up = RT_FALSE;
    app_data_set_mqtt(RT_FALSE);
    LOG_W("mqtt offline");
    RT_UNUSED(c);
}

static int snore_mqtt_start(void)
{
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;

    if (s_client.uri != RT_NULL)
        return 0;                          /* 已启动；paho 内部自管重连 */

    if (SNORE_RT_MQTT_URI[0] == '\0')
    {
        LOG_W("mqtt uri not configured, events will not be reported");
        return -RT_EINVAL;
    }

    rt_snprintf(s_client_id, sizeof(s_client_id), "wfizzz_snore_%08x",
                (rt_uint32_t)rt_tick_get());

    rt_memset(&s_client, 0, sizeof(s_client));
    s_client.uri = SNORE_RT_MQTT_URI;
    rt_memcpy(&s_client.condata, &condata, sizeof(condata));
    s_client.condata.clientID.cstring = s_client_id;
    s_client.condata.username.cstring = SNORE_RT_MQTT_USER;
    s_client.condata.password.cstring = SNORE_RT_MQTT_PASS;
    s_client.condata.keepAliveInterval = 30;
    s_client.condata.cleansession = 1;

    s_client.buf_size = s_client.readbuf_size = 1024;
    s_client.buf = rt_calloc(1, s_client.buf_size);
    s_client.readbuf = rt_calloc(1, s_client.readbuf_size);
    if (s_client.buf == RT_NULL || s_client.readbuf == RT_NULL)
    {
        LOG_E("mqtt buffer alloc failed");
        if (s_client.buf)     { rt_free(s_client.buf);     s_client.buf = RT_NULL; }
        if (s_client.readbuf) { rt_free(s_client.readbuf); s_client.readbuf = RT_NULL; }
        s_client.uri = RT_NULL;
        return -RT_ENOMEM;
    }

    s_client.online_callback  = snore_mqtt_online;
    s_client.offline_callback = snore_mqtt_offline;

    paho_mqtt_start(&s_client);
    LOG_I("mqtt started id=%s broker=%s", s_client_id, SNORE_RT_MQTT_URI);
    return 0;
}

static void snore_mqtt_publish(rt_uint16_t prob, rt_uint32_t dur_ms)
{
    char json[128];

    if (!s_mqtt_up || !s_client.isconnected)
    {
        LOG_W("event prob=%u%% but mqtt offline, dropped", (unsigned)prob);
        return;
    }

    rt_snprintf(json, sizeof(json),
                "{\"event\":\"snore\",\"prob\":%u.%02u,\"windows\":%u,"
                "\"events\":%u,\"cost_ms\":%u}",
                prob / 100, prob % 100,
                (unsigned)s_windows, (unsigned)s_events,
                (unsigned)dur_ms);

    paho_mqtt_publish(&s_client, QOS1, SNORE_RT_TOPIC, json);
    LOG_I("published %s", json);
}
#else
static int  snore_mqtt_start(void) { return -RT_ENOSYS; }
static void snore_mqtt_publish(rt_uint16_t prob, rt_uint32_t dur_ms)
{
    RT_UNUSED(prob);
    RT_UNUSED(dur_ms);
}
#endif /* PKG_USING_PAHOMQTT */

/* 记一条事件到环形历史。uptime 用开机秒数而非 RTC 时间：
 * 设备 RTC 未校时，显示绝对时间会误导。 */
static void snore_hist_add(rt_uint16_t prob, rt_uint32_t cost_ms)
{
    snore_hist_t *h = &s_hist[s_hist_head];

    h->uptime_s = (rt_uint32_t)(rt_tick_get() / RT_TICK_PER_SECOND);
    h->prob     = prob;
    h->cost_ms  = (cost_ms > 65535u) ? 65535u : (rt_uint16_t)cost_ms;

    s_hist_head = (s_hist_head + 1) % SNORE_RT_HIST_MAX;
    if (s_hist_count < SNORE_RT_HIST_MAX)
        s_hist_count++;
}

static void snore_mic_configure(rt_device_t mic)
{
    struct rt_audio_caps caps;

    rt_memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_INPUT;
    caps.sub_type  = AUDIO_DSP_PARAM;
    caps.udata.config.samplerate = SNORE_RT_SRATE;
    caps.udata.config.channels   = 1;
    caps.udata.config.samplebits = 16;
    rt_device_control(mic, AUDIO_CTL_CONFIGURE, &caps);

    rt_memset(&caps, 0, sizeof(caps));
    caps.main_type = AUDIO_TYPE_MIXER;
    caps.sub_type  = AUDIO_MIXER_VOLUME;
    caps.udata.value = SNORE_RT_GAIN;
    rt_device_control(mic, AUDIO_CTL_CONFIGURE, &caps);
}

/*
 * 采集循环的数据推进：
 *   s_win 始终是「最近 15600 个样本」的滑窗。
 *   每读到一个 chunk 就追加到窗尾；攒满后评估一次，
 *   然后把窗整体左移 SNORE_RT_HOP，为新样本腾位置。
 *   用 fill 表示窗内已有样本数，避免用 memmove 猜状态。
 */
/*
 * 一块 PCM 的 RMS 电平（0-32767）。麦克风是立体声交织，
 * 这里对全部样本求 RMS，电平量级与单声道一致，够 UI 显示用。
 */
static rt_uint32_t pcm_rms(const rt_int16_t *pcm, rt_size_t n)
{
    rt_int64_t acc = 0;
    rt_size_t i;

    if (n == 0)
        return 0;

    for (i = 0; i < n; i++)
    {
        rt_int32_t v = pcm[i];
        acc += (rt_int64_t)v * v;
    }

    return (rt_uint32_t)sqrt((double)acc / (double)n);
}

static void snore_rt_entry(void *param)
{    rt_device_t mic = RT_NULL;
    rt_uint8_t *chunk = RT_NULL;
    rt_uint32_t fill = 0;
    rt_uint32_t consec = 0;
    rt_tick_t last_event = 0;
    int ok = 0;
    int mic_acquired = 0;

    RT_UNUSED(param);

    /* snore_infer_init 返回 -4 = 已被 snore_server/snore_selftest 初始化过，
     * 属正常（模型与 arena 是进程内单例），不能当失败处理 */
    {
        int rc = snore_infer_init();
        if (rc != 0 && rc != -4)
        {
            LOG_E("infer init failed (%d)，模型未烧进固件？", rc);
            goto out;
        }
        if (rc == -4)
            LOG_I("infer 已初始化，复用现有 interpreter");
    }

    s_win = (rt_int16_t *)rt_malloc(SNORE_WIN_SAMPLES * sizeof(rt_int16_t));
    chunk = rt_malloc(SNORE_RT_CHUNK);
    if (s_win == RT_NULL || chunk == RT_NULL)
    {
        LOG_E("buffer alloc failed");
        goto out;
    }

    mic = rt_device_find(SNORE_RT_MIC);
    if (mic == RT_NULL)
    {
        LOG_E("%s not found (需 BSP_USING_AUDIO + RECORD)", SNORE_RT_MIC);
        goto out;
    }

    /* 经仲裁层拿 mic0，避免和小智/audio_event 同时 open 导致收不到数据 */
    if (mic_arb_acquire(MIC_ARB_SNORE) != RT_EOK)
    {
        LOG_W("mic0 busy, snore detect not started");
        mic = RT_NULL;
        goto out;
    }
    mic_acquired = 1;

    if (rt_device_open(mic, RT_DEVICE_OFLAG_RDONLY) != RT_EOK)
    {
        LOG_E("open %s failed", SNORE_RT_MIC);
        mic = RT_NULL;
        goto out;
    }
    snore_mic_configure(mic);

    snore_mqtt_start();
    ok = 1;
    LOG_I("started %dHz th=%u%% hop=%d consec=%d",
          SNORE_RT_SRATE, (unsigned)s_threshold,
          SNORE_RT_HOP, SNORE_RT_CONSEC);

    while (!s_stop)
    {
        rt_size_t len = rt_device_read(mic, 0, chunk, SNORE_RT_CHUNK);
        rt_size_t n, i;

        if (len == 0)
        {
            rt_thread_mdelay(1);
            continue;
        }

        n = len / sizeof(rt_int16_t);

        /* 电平条数据源：每块都更新，与推理节奏无关，UI 看起来是实时的 */
        s_rms = pcm_rms((const rt_int16_t *)chunk, n);

        for (i = 0; i < n; i++)
        {
            /* 窗口攒满就先评估再回退半窗，保证 fill 永不超过容量。
             * 早期版本先写满再检查，fill 会越过 SNORE_WIN_SAMPLES
             * 造成堆越界写（曾实测触发数百次）。 */
            if (fill >= SNORE_WIN_SAMPLES)
            {
                snore_result_t r;
                rt_uint16_t prob;

                /* 推理内部自带 mel 前端，这里不要再调 frontend */
                if (snore_infer_window(s_win, &r) != 0)
                {
                    LOG_W("infer failed");
                    fill = 0;               /* 丢弃本窗，避免卡死 */
                    continue;
                }

                s_windows++;
                prob = (rt_uint16_t)(r.snore * 100.0f + 0.5f);
                s_last_prob = prob;

                LOG_D("win=%u prob=%u%% fe=%ums inf=%ums",
                      (unsigned)s_windows, (unsigned)prob,
                      (unsigned)r.fe_ms, (unsigned)r.infer_ms);

                if (prob >= s_threshold)
                {
                    if (++consec >= SNORE_RT_CONSEC)
                    {
                        rt_tick_t now = rt_tick_get();
                        if (last_event == 0 ||
                            (now - last_event) >
                            rt_tick_from_millisecond(SNORE_RT_COOLDOWN_MS))
                        {
                            last_event = now;
                            s_events++;
                            snore_hist_add(prob, r.fe_ms + r.infer_ms);
                            /* 落盘归档：非阻塞投递，真正写 SD 在归档线程 */
                            snore_log_append(prob, r.fe_ms + r.infer_ms);
                            LOG_I("SNORE #%u prob=%u%% fe=%ums inf=%ums",
                                  (unsigned)s_events, (unsigned)prob,
                                  (unsigned)r.fe_ms, (unsigned)r.infer_ms);
                            snore_mqtt_publish(prob, r.fe_ms + r.infer_ms);
                        }
                        consec = 0;
                    }
                }
                else
                {
                    consec = 0;
                }

                /* 左移半窗，保留后半段（下个窗口与之重叠） */
                memmove(s_win, s_win + SNORE_RT_HOP,
                        (SNORE_WIN_SAMPLES - SNORE_RT_HOP) * sizeof(rt_int16_t));
                fill = SNORE_WIN_SAMPLES - SNORE_RT_HOP;
            }

            s_win[fill++] = ((rt_int16_t *)chunk)[i];
        }

        rt_thread_mdelay(1);      /* 推理很重，让低优先级线程有机会跑 */
    }

    LOG_I("stopped, windows=%u events=%u",
          (unsigned)s_windows, (unsigned)s_events);

out:
    if (mic != RT_NULL)
        rt_device_close(mic);
    if (mic_acquired)
        mic_arb_release(MIC_ARB_SNORE);
    if (s_win)   { rt_free(s_win);   s_win = RT_NULL; }
    if (chunk)   { rt_free(chunk);   chunk = RT_NULL; }

    /* 把队列里剩余的事件落盘再退出。
     * 放在这里而不是 snore_rt_stop()：stop 只是置标志，线程可能还在
     * 处理最后一窗，此时队列里可能已有未写完的记录。 */
    snore_log_stop();

    s_running = RT_FALSE;
    s_stop = RT_FALSE;
    s_thread = RT_NULL;
    s_rms = 0;                    /* 停止后电平归零，避免 UI/msh 显示残留值 */
    if (!ok)
        LOG_W("exited before start");
}

static int snore_rt_cmd(int argc, char **argv)
{
    const char *cmd = (argc > 1) ? argv[1] : "status";

    if (rt_strcmp(cmd, "start") == 0)
    {
        if (snore_rt_start() != 0)
            return -RT_ERROR;
        return 0;
    }

    if (rt_strcmp(cmd, "stop") == 0)
    {
        snore_rt_stop();
        return 0;
    }

    if (rt_strcmp(cmd, "th") == 0)
    {
        if (argc < 3)
        {
            rt_kprintf("usage: snore_rt th <0-100>\n");
            return -RT_ERROR;
        }
        snore_rt_set_threshold((rt_uint16_t)strtoul(argv[2], RT_NULL, 0));
        return 0;
    }

    if (rt_strcmp(cmd, "pub") == 0)
    {
        snore_rt_publish_test();
        return 0;
    }

    {
        snore_rt_status_t st;
        snore_rt_get_status(&st);
        rt_kprintf("[snore_rt] running=%s mqtt=%s windows=%u events=%u "
                   "prob=%u%% th=%u%% rms=%u\n",
                   st.running ? "yes" : "no",
                   st.mqtt_up ? "online" : "offline",
                   (unsigned)st.windows, (unsigned)st.events,
                   (unsigned)st.last_prob, (unsigned)st.threshold,
                   (unsigned)st.rms);
    }
    return 0;
}

/* ---------------- 对外接口（msh 与 UI 抽屉共用） ---------------- */

void snore_rt_get_status(snore_rt_status_t *st)
{
    if (st == RT_NULL)
        return;

    st->running    = s_running;
    st->mqtt_up    = s_mqtt_up;
    st->threshold  = s_threshold;
    st->last_prob  = s_last_prob;
    st->windows    = s_windows;
    st->events     = s_events;
    st->rms        = s_rms;
}

int snore_rt_start(void)
{
    if (s_running)
    {
        rt_kprintf("[snore_rt] already running\n");
        return 0;
    }

    /* 让仲裁器在被小智抢占时能异步叫停本线程 */
    mic_arb_set_stop_cb(MIC_ARB_SNORE, snore_rt_stop);

    /* 归档线程与检测线程同生命周期：不检测就不会产生事件，
     * 让其常驻后台只会占用一个线程槽。 */
    snore_log_start();

    s_stop = RT_FALSE;
    s_running = RT_TRUE;
    s_thread = rt_thread_create("snore_rt", snore_rt_entry, RT_NULL,
                                SNORE_RT_STACK, SNORE_RT_PRIO, 20);
    if (s_thread == RT_NULL)
    {
        s_running = RT_FALSE;
        rt_kprintf("[snore_rt] thread create failed\n");
        return -RT_ENOMEM;
    }

    rt_thread_startup(s_thread);
    return 0;
}

void snore_rt_stop(void)
{
    if (s_running)
    {
        s_stop = RT_TRUE;
        rt_kprintf("[snore_rt] stopping...\n");
    }
}

void snore_rt_set_threshold(rt_uint16_t th)
{
    if (th > 100)
        th = 100;
    s_threshold = th;
    rt_kprintf("[snore_rt] threshold=%u%%\n", (unsigned)s_threshold);
}

/* 按时间倒序导出历史：最新一条在前。返回条目数。 */
int snore_rt_get_history(snore_rt_hist_t *buf, int max)
{
    int n, i;

    if (buf == RT_NULL || max <= 0)
        return 0;

    n = (s_hist_count < (rt_uint32_t)max) ? (int)s_hist_count : max;
    if (n <= 0)
        return 0;

    /* s_hist_head 指向下一条待写位置，故最新一条在 head-1 */
    for (i = 0; i < n; i++)
    {
        rt_uint32_t idx = (s_hist_head + SNORE_RT_HIST_MAX - 1u - (rt_uint32_t)i)
                          % SNORE_RT_HIST_MAX;

        buf[i].uptime_s = s_hist[idx].uptime_s;
        buf[i].prob     = s_hist[idx].prob;
        buf[i].cost_ms  = s_hist[idx].cost_ms;
    }
    return n;
}

void snore_rt_publish_test(void)
{
    snore_mqtt_publish(99, 0);
    rt_kprintf("[snore_rt] test event sent (mqtt=%s)\n",
               s_mqtt_up ? "online" : "offline");
}

MSH_CMD_EXPORT_ALIAS(snore_rt_cmd, snore_rt, snore realtime detect (mic -> yamnet -> mqtt));
