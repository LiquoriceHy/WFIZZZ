/*
 * app_data.c - 智能终端数据中枢（结构定义见 app_data.h）
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>       /* GET_PIN（同 demo_adc.c），电池门控脚要用 */
#include <sys/time.h>     /* time_t，与 RTC 驱动同款头（无 LIBC 配置也可用） */

#ifdef RT_USING_WIFI
#include <wlan_mgnt.h>
#endif
#ifdef RT_USING_NETDEV
#include <lwip/ip_addr.h> /* ip_addr_t / ip_addr_isany / ipaddr_ntoa_r，须先于 netdev.h */
#include <netdev.h>
#endif
#ifdef PKG_USING_CPU_USAGE
#include <cpu_usage.h>    /* cpu_load_average()，返回 0-100 占用率 */
#endif
#ifdef PKG_NETUTILS_NTP
#include <ntp.h>          /* ntp_sync_to_rtc()，netutils 包的 NTP 组件 */
#endif

#include "app_data.h"
#include "drv_ipc.h"    /* 跨核温湿度：ipc1 收 M33 帧（edge_rc_frame_t） */
#include "data_store.h" /* 便签/闹钟的延迟落盘：本文件的 1 秒循环负责触发 */

#define AD_THREAD_STACK  2048
#define AD_THREAD_PRIO   24      /* 低于 LVGL(21)/音频采集(20)，纯慢速轮询 */
#define AD_PERIOD_MS     1000

/* RTC 驱动（rt-thread/components/drivers/rtc/rtc.c）导出的 UTC 时间戳 */
extern rt_err_t get_timestamp(time_t *timestamp);

/* 显示时区：与 msh date 命令一致（rtconfig: RT_LIBC_TZ_DEFAULT_HOUR=8） */
#ifndef RT_LIBC_TZ_DEFAULT_HOUR
#define RT_LIBC_TZ_DEFAULT_HOUR 8
#define RT_LIBC_TZ_DEFAULT_MIN  0
#endif
#define AD_TZ_OFFSET_SEC  ((RT_LIBC_TZ_DEFAULT_HOUR * 3600) + (RT_LIBC_TZ_DEFAULT_MIN * 60))

/* 2020-01-01 之前的 RTC 时间视为"未设置"（软 RTC 开机从 1970 起跳） */
#define AD_TIME_VALID_MIN 1577836800LL

volatile app_data_t g_app;

/* ---------- 预留 setter（当前无调用者，以后由外设模块点亮 UI） ---------- */

void app_data_set_temp_hum(float temp_c, float hum_pct)
{
    g_app.temp_c = temp_c;
    g_app.hum_pct = hum_pct;
    g_app.temp_hum_valid = RT_TRUE;
}

void app_data_set_weather(const char *text)
{
    rt_strncpy((char *)g_app.weather_text, text ? text : "",
               sizeof(g_app.weather_text) - 1);
    g_app.weather_text[sizeof(g_app.weather_text) - 1] = '\0';
}

void app_data_set_mqtt(rt_bool_t online)
{
    g_app.mqtt_online = online;
}

void app_data_set_ntp_synced(void)
{
    g_app.ntp_synced = RT_TRUE;
}

/* 电量映射端点：3300mV 视为空、4200mV 视为满（无电量计的线性近似，
 * 锂电实际放电曲线非线性，精度要求高时再换分段表） */
#define BATT_MV_EMPTY 3300
#define BATT_MV_FULL  4200

/* 电池端电压（mV）入库；电量百分比在此换算，UI 只读 */
void app_data_set_batt(rt_uint16_t batt_mv)
{
    rt_uint32_t pct;

    if (batt_mv <= BATT_MV_EMPTY)
        pct = 0;
    else if (batt_mv >= BATT_MV_FULL)
        pct = 100;
    else
        pct = ((rt_uint32_t)(batt_mv - BATT_MV_EMPTY) * 100U) /
              (BATT_MV_FULL - BATT_MV_EMPTY);

    g_app.batt_mv    = batt_mv;
    g_app.batt_pct   = (rt_uint8_t)pct;
    g_app.batt_valid = RT_TRUE;
}

/* 整组写入：先数值后状态，UI 读到 OK 时数值必已完整 */
void app_data_set_memo(const app_data_memo_t *m)
{
    if (m == RT_NULL)
        return;

    g_app.memo.done   = m->done;
    g_app.memo.total  = m->total;
    g_app.memo.known  = m->known;
    g_app.memo.fuzzy  = m->fuzzy;
    g_app.memo.forgot = m->forgot;
    g_app.memo.state  = m->state;
}

/* ---------- 小智聊天 setter（队列写入，UI 侧 app_data_xz_pop 排空） ---------- */

void app_data_set_xiaozhi_msg(rt_uint8_t role, const char *text)
{
    if (text == RT_NULL || text[0] == '\0')
        return;

    /* 临界区内完成"拷贝 + 推进 head + count++"，否则 UI 侧可能在
     * 文本只拷了一半时读到该槽（count 已增但内容是旧的）。 */
    rt_enter_critical();

    if (g_app.xiaozhi_count >= APP_DATA_XZ_Q_LEN)
    {
        /* 队列满：丢最旧一条，保留最新。对话里刚说的那句更重要。 */
        g_app.xiaozhi_tail =
            (rt_uint8_t)((g_app.xiaozhi_tail + 1) % APP_DATA_XZ_Q_LEN);
        g_app.xiaozhi_count--;
    }

    g_app.xiaozhi_role[g_app.xiaozhi_head] = role;
    rt_strncpy(g_app.xiaozhi_q[g_app.xiaozhi_head], text,
               APP_DATA_XZ_TEXT_LEN - 1);
    g_app.xiaozhi_q[g_app.xiaozhi_head][APP_DATA_XZ_TEXT_LEN - 1] = '\0';

    g_app.xiaozhi_head =
        (rt_uint8_t)((g_app.xiaozhi_head + 1) % APP_DATA_XZ_Q_LEN);
    g_app.xiaozhi_count++;
    g_app.xiaozhi_seq++;   /* 累计条数，仅统计用 */

    rt_exit_critical();
}

rt_bool_t app_data_xz_pop(rt_uint8_t *role, char *buf, rt_size_t len)
{
    rt_bool_t got = RT_FALSE;

    if (buf == RT_NULL || len == 0)
        return RT_FALSE;

    rt_enter_critical();
    if (g_app.xiaozhi_count > 0)
    {
        rt_uint8_t tail = g_app.xiaozhi_tail;

        if (role != RT_NULL)
            *role = g_app.xiaozhi_role[tail];

        rt_strncpy(buf, g_app.xiaozhi_q[tail], len - 1);
        buf[len - 1] = '\0';

        g_app.xiaozhi_tail =
            (rt_uint8_t)((tail + 1) % APP_DATA_XZ_Q_LEN);
        g_app.xiaozhi_count--;
        got = RT_TRUE;
    }
    rt_exit_critical();

    return got;
}

int app_data_xz_pending(void)
{
    int n;

    rt_enter_critical();
    n = (int)g_app.xiaozhi_count;
    rt_exit_critical();

    return n;
}

void app_data_set_xiaozhi_status(const char *text)
{
    rt_strncpy((char *)g_app.xiaozhi_status, text ? text : "",
               sizeof(g_app.xiaozhi_status) - 1);
    g_app.xiaozhi_status[sizeof(g_app.xiaozhi_status) - 1] = '\0';
}

/* ---------- 闹钟 setter（alarm.c 线程与 ui_alarm 共用） ---------- */

void app_data_set_alarm(int idx, const app_data_alarm_t *a)
{
    if (idx < 0 || idx >= APP_ALARM_NUM || a == RT_NULL)
        return;

    g_app.alarms[idx].hour   = a->hour;
    g_app.alarms[idx].min    = a->min;
    g_app.alarms[idx].days   = a->days;
    g_app.alarms[idx].enable = a->enable;   /* 最后写 enable：先数值后状态 */
}

void app_data_set_alarm_ring(rt_bool_t on, rt_int64_t start_ts)
{
    if (on)
        g_app.alarm_ring_ts = start_ts;
    g_app.alarm_ring = on;
}

/* ---------- 每秒轮询 ---------- */

static void ad_poll_wifi(void)
{
#ifdef RT_USING_WIFI
    g_app.wifi_connected = rt_wlan_is_connected();
    if (g_app.wifi_connected)
    {
        g_app.wifi_valid = RT_TRUE;
        g_app.wifi_rssi = rt_wlan_get_rssi();
    }
    else
    {
        g_app.wifi_valid = RT_FALSE;
    }
#else
    g_app.wifi_connected = RT_FALSE;
    g_app.wifi_valid = RT_FALSE;
#endif
}

static void ad_poll_netdev(void)
{
#ifdef RT_USING_NETDEV
    struct netdev *nd = netdev_get_by_name("w0");   /* wlan_lwip.c 注册名 */

    if (nd == RT_NULL)
        nd = netdev_default;

    if (nd != RT_NULL && !ip_addr_isany(&nd->ip_addr))
    {
        ipaddr_ntoa_r(&nd->ip_addr, (char *)g_app.ip_str, sizeof(g_app.ip_str));
        ipaddr_ntoa_r(&nd->gw, (char *)g_app.gw_str, sizeof(g_app.gw_str));
        return;
    }
#endif
    rt_strcpy((char *)g_app.ip_str, "--");
    rt_strcpy((char *)g_app.gw_str, "--");
}

static void ad_poll_time(void)
{
#ifdef RT_USING_RTC
    time_t ts = 0;

    if (get_timestamp(&ts) == RT_EOK)
    {
        g_app.time_ts = (rt_int64_t)ts + AD_TZ_OFFSET_SEC;
        g_app.time_valid = (ts >= AD_TIME_VALID_MIN);
    }
    else
    {
        g_app.time_valid = RT_FALSE;
    }
#else
    g_app.time_valid = RT_FALSE;
#endif
}

/* NTP 校时：独立线程，不放 app_data 的 1 秒轮询里。
 *
 * 为什么不放在 1 秒循环里：ntp_sync_to_rtc() 是同步阻塞的 DNS 解析 + UDP
 * 收包，NTP_GET_TIMEOUT = 5 秒（packages/netutils-latest/ntp/ntp.c:181）。
 * 一旦 NTP 服务器不可达，整个 app_data 线程会被卡 5 秒——而它还负责时钟、
 * 电量、WiFi 状态刷新，表现为状态栏时间冻结、电量不更新。
 *
 * 无网络时 DNS 解析也可能阻塞，所以先等 wifi_connected 再发请求。
 * 成功后线程自行退出：校时只需一次，RTC 会自己走。
 * 未开 PKG_NETUTILS_NTP 时整个线程体为空，直接退出。 */
#define AD_NTP_RETRY_MS 60000
#define AD_NTP_STACK    2048
#define AD_NTP_PRIO     27      /* 低于 app_data(24+) 与 UI(21)，纯补漏 */

static void ad_ntp_entry(void *parameter)
{
#ifdef PKG_NETUTILS_NTP
    RT_UNUSED(parameter);

    while (!g_app.ntp_synced)
    {
        /* 没连网就别发请求：DNS 会阻塞，白等一轮超时 */
        if (!g_app.wifi_connected)
        {
            rt_thread_mdelay(1000);
            continue;
        }

        if (ntp_sync_to_rtc("ntp.aliyun.com") > 0)
        {
            app_data_set_ntp_synced();
            rt_kprintf("[app_data] NTP synced\n");
            return;
        }

        rt_kprintf("[app_data] NTP sync failed, retry in %ds\n",
                   AD_NTP_RETRY_MS / 1000);
        rt_thread_mdelay(AD_NTP_RETRY_MS);
    }
#else
    RT_UNUSED(parameter);
#endif
}

static void ad_poll_sysinfo(void)
{
    rt_size_t total, used, max_used;

    g_app.uptime_s = rt_tick_get_millisecond() / 1000U;

#ifdef PKG_USING_CPU_USAGE
    /* cpu_load_average() 每 1s 才刷新一档，四舍五入到整数百分比 */
    g_app.cpu_used_pct = (rt_uint8_t)(cpu_load_average() + 0.5f);
#else
    g_app.cpu_used_pct = 0;
#endif

    rt_memory_info(&total, &used, &max_used);
    g_app.mem_used_pct = (total > 0) ? (rt_uint8_t)((used * 100) / total) : 0;
    g_app.mem_free_kb = (rt_uint32_t)((total - used) / 1024);
}

/* 诊断开关：置 1 则跳过电池 ADC 采样。
 *
 * 2026-09-28 曾置 1 用于隔离"M55 挂载 Flash 必败"的诱因，因为电池首次
 * 采样与挂载同时刻并发，是当时最后一个未排除的 XiaoZhi 独有活动。
 * 现已验证两者无关，恢复采样：实际原因是开机时对共享 NOR 的 lfs mkfs
 * 擦除风暴打断 CM33 XIP，已在 mnt.c 中用 BSP_USING_XiaoZhi 跳过。 */
#define AD_SKIP_BATT 0

/* ---------- 跨核温湿度（M33 AHT20 经硬件 IPC 推送，需 BSP_USING_IPC） ----------
 * 2026-09-28 起弃用 SOCMEM 直访方案：实测双核并发访问 m33_m55_shared
 * 端口会使 CM33 挂死（读探针正常、写探针挂死、轮询期双核流量必然挂死）。
 * 改由 ipc1 设备（CM33 pipe0 到 CM55）接收帧，帧内 channel[0]=temp_x10、
 * [1]=hum_x10、[2:3]=tick_ms、[4]=0xA020 帧类型标记。 */

#define TH_STALE_LIMIT 30   /* 连续 30s 无帧视为 M33 侧停更，UI 回落占位 */
#define TH_FRAME_TAG   0xA020U

static rt_bool_t   th_online_logged = RT_FALSE;
static rt_uint32_t th_silent_s = 0;

static void th_rx_frame(const edge_rc_frame_t *f)
{
    rt_int16_t  temp_x10 = (rt_int16_t)f->channel[0];
    rt_uint16_t hum_x10  = f->channel[1];

    /* 包驱动失败哨兵（温度 -50/湿度 0）被量程校验一并过滤 */
    if (temp_x10 < -400 || temp_x10 > 850 || hum_x10 <= 0 || hum_x10 > 1000)
        return;

    app_data_set_temp_hum(temp_x10 / 10.0f, hum_x10 / 10.0f);
    th_silent_s = 0;
    if (!th_online_logged)
    {
        th_online_logged = RT_TRUE;
        rt_kprintf("[app_data] aht20 online via ipc, t10=%d h10=%d\n",
                   temp_x10, hum_x10);
    }
}

static void th_rx_entry(void *parameter)
{
    rt_device_t rx;
    edge_rc_frame_t f;

    RT_UNUSED(parameter);

    if (edge_ipc_device_register() != RT_EOK)
    {
        rt_kprintf("[app_data] ipc device register failed\n");
        return;
    }
    rx = edge_ipc_device_find(EDGE_IPC0_DEVICE_NAME);   /* "ipc1"：收 M33 pipe0 帧 */
    if (rx == RT_NULL || rt_device_open(rx, RT_DEVICE_OFLAG_RDWR) != RT_EOK)
    {
        rt_kprintf("[app_data] ipc rx device not available\n");
        return;
    }

    while (1)
    {
        while (rt_device_read(rx, 0, &f, 1) == 1)
        {
            if (f.magic == RC_MAGIC_WORD && f.role == RC_ROLE_M33 &&
                f.channel[4] == TH_FRAME_TAG &&
                edge_rc_checksum(&f) == f.checksum)
            {
                th_rx_frame(&f);
            }
        }

        th_silent_s += 1;
        if (th_silent_s == TH_STALE_LIMIT && g_app.temp_hum_valid)
        {
            g_app.temp_hum_valid = RT_FALSE;
            th_online_logged = RT_FALSE;   /* 恢复后允许重新打 online 日志 */
        }
        rt_thread_mdelay(1000);
    }
}

static int th_rx_start(void)
{
    rt_thread_t tid = rt_thread_create("aht20ipc", th_rx_entry, RT_NULL,
                                       2048, 26, 10);
    if (tid == RT_NULL)
        return -RT_ERROR;
    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(th_rx_start);

/* ---------- 本地电池电压（ADC1 CH1，分压 + NMOS 门控，见 M33 工程 figures/adc_1.png） ----------
 * VBAT → 10k → P15.5（CH1）→ 7.5k → Q5(NMOS) → GND，Q5 栅极由 P8.4 控制，
 * 平时断开省电；drv_adc_read 返回分压节点 mV（内部 CountsTo_mVolts，vref=1800），
 * 电池端电压 = 节点 × 4200/1800（= 分压比 7/3，4.2V 满电恰打满量程）。
 * ADC 框架与 M33 侧共享硬件，仅本核使用（勿与 M33 demo_adc 同时采样）。 */

#define BATT_ADC_DEV_NAME "adc1"
#define BATT_ADC_CHANNEL  1             /* 板上唯一接电池分压的通道（P15.5，勿用 0） */
#define BATT_CTRL_PIN     GET_PIN(8, 4) /* ADC_CTRL：高=接通分压回路 */
#define BATT_SAMPLE_EVERY 30            /* 每轮询 30 次（约 30s）采一次，首次立即采 */
#define BATT_SETTLE_MS    5             /* 门控接通后等 RC（10nF）稳定，同 demo_adc */
#define BATT_DROP_FIRST   2             /* 丢弃建立期样本 */
#define BATT_SAMPLE_NUM   8             /* 平均样本数 */
#define BATT_NODE_MV_MIN  500           /* 节点合理区：4.2V 满电=1800，滤未装电池的悬空读数 */
#define BATT_NODE_MV_MAX  1900

static rt_adc_device_t batt_dev        = RT_NULL;
static rt_bool_t       batt_dev_ready  = RT_FALSE;
static rt_bool_t       batt_dev_failed = RT_FALSE;
static rt_uint8_t      batt_poll_cnt   = 0;
static rt_bool_t       batt_online_logged = RT_FALSE;

static void ad_poll_batt(void)
{
    /* 电压变化慢，降到约 30s 一次；首次（batt_valid=0）立即采一帧 */
    if (++batt_poll_cnt < BATT_SAMPLE_EVERY && g_app.batt_valid)
        return;
    batt_poll_cnt = 0;

    if (!batt_dev_ready)
    {
        if (batt_dev_failed)
            return;
        batt_dev = (rt_adc_device_t)rt_device_find(BATT_ADC_DEV_NAME);
        if (batt_dev == RT_NULL)
        {
            batt_dev_failed = RT_TRUE;   /* 只报一次，UI 保持占位 */
            rt_kprintf("[app_data] %s not found, battery disabled\n", BATT_ADC_DEV_NAME);
            return;
        }
        rt_adc_enable(batt_dev, BATT_ADC_CHANNEL);
        rt_pin_mode(BATT_CTRL_PIN, PIN_MODE_OUTPUT);
        batt_dev_ready = RT_TRUE;
    }

    /* 分压回路平时断开（Q5 关断省电），采样窗口内接通，采完立即断开 */
    rt_pin_write(BATT_CTRL_PIN, PIN_HIGH);
    rt_thread_mdelay(BATT_SETTLE_MS);

    rt_uint32_t acc = 0;
    int n = 0;
    for (int i = 0; i < BATT_DROP_FIRST + BATT_SAMPLE_NUM; i++)
    {
        rt_uint32_t v = rt_adc_read(batt_dev, BATT_ADC_CHANNEL);   /* 节点 mV */
        if (i >= BATT_DROP_FIRST)
        {
            acc += v;
            n++;
        }
    }
    rt_pin_write(BATT_CTRL_PIN, PIN_LOW);

    rt_uint32_t node_mv = acc / n;
    if (node_mv < BATT_NODE_MV_MIN || node_mv > BATT_NODE_MV_MAX)
        return;   /* 未装电池/读数异常，保持上次值或占位 */

    /* +900/1800 = +0.5 四舍五入，同 demo_adc 换算 */
    rt_uint16_t batt_mv = (rt_uint16_t)((node_mv * 4200U + 900U) / 1800U);
    app_data_set_batt(batt_mv);

    if (!batt_online_logged)
    {
        batt_online_logged = RT_TRUE;
        rt_kprintf("[app_data] battery online, node=%u mV batt=%u mV\n",
                   node_mv, (unsigned int)batt_mv);
    }
}

static void ad_entry(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        ad_poll_wifi();
        ad_poll_netdev();
        ad_poll_time();
        /* NTP 不在这里：它是阻塞调用（最长 5 秒），见 ad_ntp_entry 的注释 */
        ad_poll_sysinfo();
#if !AD_SKIP_BATT
        ad_poll_batt();
#endif
        /* 便签与闹钟的延迟落盘：写操作（UI 保存、AI 工具）只置脏标志，
         * 实际写 SD 在此处执行。放在 1 秒循环中的理由见 data_store.h：
         * MCP 工具回调运行于 websocket 线程，同步写卡会阻塞音频上行。
         * 本函数内部有 3 秒去抖，无脏标志时立即返回，开销可忽略。 */
        data_store_poll();

        rt_thread_mdelay(AD_PERIOD_MS);
    }
}

int app_data_start(void)
{
    rt_thread_t tid;

    tid = rt_thread_create("appdat", ad_entry, RT_NULL,
                           AD_THREAD_STACK, AD_THREAD_PRIO, 10);
    if (tid == RT_NULL)
    {
        rt_kprintf("[app_data] thread create failed\n");
        return -RT_ERROR;
    }
    rt_thread_startup(tid);

    /* NTP 独立线程：失败时能自持 5 秒超时而不拖累 1 秒轮询。
     * 未开包时线程体立即返回，不会留下空转线程。 */
    tid = rt_thread_create("ntpsync", ad_ntp_entry, RT_NULL,
                           AD_NTP_STACK, AD_NTP_PRIO, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
    else
        rt_kprintf("[app_data] ntp thread create failed\n");

    return RT_EOK;
}
INIT_APP_EXPORT(app_data_start);
