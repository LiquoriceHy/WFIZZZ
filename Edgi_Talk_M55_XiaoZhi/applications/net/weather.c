/*
 * weather.c - 心知天气（Seniverse）拉取线程
 *
 * 低优先级线程每 WEA_PERIOD_MS 拉取一次实况并写入 g_app.weather_text，
 * UI（ui_dashboard.c）用 lv_timer 每秒读取刷新，与 maimemo.c、wifi_auto.c
 * 采用相同的"线程写，UI 拉取"模式。
 *
 * 不能在 LVGL 线程中执行的原因：LVGL 线程栈仅 24KB，而 mbedTLS 的
 * in/out 记录缓冲各 16KB，必然栈溢出。TLS 请求一律在独立线程中发送。
 *
 * 选择心知而非和风的原因（2026-10-26 实测，详见 docs/qweather-integration.md）：
 *   1) 响应不压缩。无论是否携带 Accept-Encoding，响应都没有
 *      Content-Encoding，body 前两字节直接为 "{"（0x7b 0x22）。板上没有
 *      zlib/inflate，和风强制 gzip 会使该方案完全不可用。
 *   2) 证书链为 api.seniverse.com，RapidSSL TLS RSA CA G1，
 *      至 DigiCert Global Root G2，正是 tls_certificate.c 中已嵌入的根证书。
 *      和风专属域名使用 Let's Encrypt 的 ISRG Root X1，板内没有该根证书。
 *   3) 无需专属 host，无需修改 tls_certificate.c，无需改动 Kconfig。
 *
 * 认证方式为私钥直传 ?key=<私钥>，不使用签名：
 *   签名要求设备端计算 HMAC-SHA1 并依赖准确的 10 位秒级 ts，而本机
 *   RTC 开机时被重置为 2024-01-01 且未启用 NTP（见 m55-alarm-feature），
 *   ts 一旦错误签名必然失败，等于复制一份墨墨那种"依赖 HTTP Date 校时"的
 *   脆弱依赖。私钥经 HTTPS 传输，落于 SD 卡且不入库。若将来改用签名，
 *   参数名是 uid（不是 public_key），待签串仅包含 ts 与 uid。
 *
 * 免费版 now 仅返回 text、code、temperature 三个字段，不含湿度、风、气压
 * 与体感。因此此处只取温度与天气现象文字。
 * 温湿度读数另有本地 AHT20 链路（app_data_set_temp_hum），两者不冲突。
 *
 * 语言固定为 en：屏上仅有 ASCII 字体（lv_conf 中 SIMSUN 子集仅 1118 字，
 * 缺少"网络连接"这类常用字），中文天气文案会显示为空白方块。
 * 免费版 en 文案长度不固定，weather_text 仅 24 字节，写入前必须截断。
 */
#include <rtthread.h>
#include <rtdevice.h>

#if defined(PKG_USING_WEBCLIENT) && defined(PKG_USING_CJSON) && defined(RT_USING_NETDEV)

#include <lwip/ip_addr.h>   /* ip_addr_t / ip_addr_isany，须先于 netdev.h */
#include <netdev.h>

#include <webclient.h>
#include <cJSON.h>

#include <stdlib.h>         /* atoi */

#include "app_cfg.h"
#include "app_data.h"

/* ---------- 参数 ---------- */

#define WEA_THREAD_STACK   12288   /* mbedTLS RSA 验签在栈上放多个 mbedtls_mpi（各 1KB+），
                                    * 与 maimemo 同档。低于 10KB 握手期有栈溢出风险。 */
#define WEA_THREAD_PRIO    26      /* 低于 LVGL(21)/音频(20)/app_data(24)/maimemo(25) */

/* 实况 15 分钟一次。心知免费版限 1 次/秒、10 万次/月；
 * 15 分钟 = 96 次/天 ≈ 2900 次/月，远在额度内，且天气本身也不值得更密。 */
#define WEA_PERIOD_MS      (15 * 60 * 1000)

/* 未配置 key 时降低轮询频率并保持静默，用户于网页填写后下一轮自然生效，无需重启 */
#define WEA_NO_KEY_MS      (30 * 1000)

/* 响应体上限。now 的实测明文约 200-300 字节，4KB 已非常宽松；
 * 不设 16KB 的起始值是为了避免每次分配大块内存，因为主堆峰值已达 92%
 * （见 app_cfg.h）。 */
#define WEA_RESP_MAX       4096

/* 失败退避：1s/4s/15s/60s，达到上限后固定为 60s。
 * 心知没有和风那种"错误请求视为攻击并冻结账号"的条款，但退避仍是良好
 * 实践，也可避免 key 填错时大量输出日志。 */
static const rt_uint32_t wea_retry_ms[] = { 1000, 4000, 15000, 60000 };
#define WEA_RETRY_N        (sizeof(wea_retry_ms) / sizeof(wea_retry_ms[0]))

/* ---------- 网络就绪 ---------- */

static rt_bool_t wea_net_ready(void)
{
    struct netdev *nd = netdev_get_by_name("w0");   /* wlan_lwip.c 注册名 */

    if (nd == RT_NULL)
        nd = netdev_default;

    return (nd != RT_NULL && netdev_is_up(nd) &&
            netdev_is_link_up(nd) && !ip_addr_isany(&nd->ip_addr));
}

/* ---------- HTTP ---------- */

struct wea_buf
{
    char     *data;
    rt_size_t len;
};

/* GET 并把响应体收进 out。返回 HTTP 状态码，<0 表示本地失败。 */
static int wea_get_collect(const char *url, struct wea_buf *out)
{
    struct webclient_session *session;
    char *buf;
    rt_size_t cap = 512;
    int status;

    out->data = RT_NULL;
    out->len  = 0;

    session = webclient_session_create(WEBCLIENT_HEADER_BUFSZ);
    if (session == RT_NULL)
        return -1;

    status = webclient_get(session, url);
    if (status != 200)
    {
        /* 非 200 同样需要读取 body：心知的错误说明位于 body 中
         * （{"status":"...","status_code":"AP01xxxx"}），仅打印状态码
         * 会使用户无法判断是 key 错误还是余额不足。 */
        if (status > 0)
        {
            char eb[192];
            int  n = webclient_read(session, (unsigned char *)eb, sizeof(eb) - 1);

            if (n > 0)
            {
                eb[n] = '\0';
                rt_kprintf("[weather] HTTP %d: %s\n", status, eb);
            }
            else
            {
                rt_kprintf("[weather] HTTP %d (no body)\n", status);
            }
        }
        else
        {
            /* 负数 = 请求根本没发出去或 TLS 没握手成功（DNS 失败、
             * 连不上、证书验不过、内存不足都会走到这里）。
             * 必须打出来，否则串口只有反复的 fetch 行，完全看不出
             * 是 key 错了还是网络/证书问题。 */
            rt_kprintf("[weather] request failed, status=%d\n", status);
        }
        webclient_close(session);
        return (status > 0) ? status : -1;
    }

    buf = rt_malloc(cap);
    if (buf == RT_NULL)
    {
        webclient_close(session);
        return -1;
    }

    while (1)
    {
        rt_size_t got;

        if (out->len + 1 >= cap)
        {
            char    *nb;
            rt_size_t ncap = cap * 2;

            if (ncap > WEA_RESP_MAX)
                ncap = WEA_RESP_MAX;
            if (ncap <= cap)                /* 已到上限，无法继续收 */
            {
                rt_kprintf("[weather] response over %d bytes\n", (int)WEA_RESP_MAX);
                rt_free(buf);
                webclient_close(session);
                return -2;
            }

            nb = rt_realloc(buf, ncap);
            if (nb == RT_NULL)
            {
                rt_free(buf);
                webclient_close(session);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }

        got = webclient_read(session, (unsigned char *)buf + out->len, cap - out->len - 1);
        if (got <= 0)
            break;
        out->len += got;
    }

    buf[out->len] = '\0';
    out->data = buf;

    webclient_close(session);
    return 200;
}

/* ---------- URL 拼装 ---------- */

/* 由 host + path + city + key 拼出完整 URL。
 *
 * 参数固定为 language=en&unit=c：
 *   en 是屏上字体所限（见文件头），unit=c 是摄氏度。
 * 城市用拼音名（心知支持），避免 URL 里出现 UTF-8 多字节需要编码。 */
static int wea_build_url(char *url, rt_size_t size)
{
    const char *host = app_cfg_get(APP_CFG_KEY_WEA_HOST);
    const char *path = app_cfg_get(APP_CFG_KEY_WEA_PATH);
    const char *city = app_cfg_get(APP_CFG_KEY_WEA_CITY);
    const char *key  = app_cfg_get(APP_CFG_KEY_WEA_KEY);

    if (host == RT_NULL || host[0] == '\0' ||
        path == RT_NULL || path[0] == '\0' ||
        key  == RT_NULL || key[0]  == '\0')
        return -1;

    if (city == RT_NULL || city[0] == '\0')
        city = "beijing";

    /* path 中可能已带 '?'（用户自行填写了参数），据此决定使用 & 还是 ?。
     * 注意使用标准 strchr 而非 rt_strchr，因为 RT-Thread 未提供后者
     * （rtthread.h 中只有 strstr、strcpy、strncpy、strcmp、strlen 等，
     * 没有 strchr 的 rt_ 前缀包装）。 */
    if (strchr(path, '?') != RT_NULL)
    {
        if (rt_snprintf(url, size, "https://%s%s&key=%s&location=%s&language=en&unit=c",
                        host, path, key, city) >= (int)size)
            return -1;
    }
    else
    {
        if (rt_snprintf(url, size, "https://%s%s?key=%s&location=%s&language=en&unit=c",
                        host, path, key, city) >= (int)size)
            return -1;
    }
    return 0;
}

/* ---------- 解析 ---------- */

/* 从心知响应里取 now.temperature 与 now.text。
 *
 * 响应结构：{"results":[{"location":{...},"now":{"text":"Clear","code":"0",
 *            "temperature":"20"},"last_update":"..."}]}
 *
 * 使用 cJSON 全量解析是安全的：响应仅约 200-300 字节，解析树远小于
 * maimemo 那种 184KB 的响应，无需像 maimemo.c 那样手工统计关键字。
 *
 * 免费版仅保证 text、code、temperature 三项存在，其余字段（湿度、风、
 * 气压）可能缺失，因此每个字段都必须判断 RT_NULL 后再取值，
 * 直接 cJSON_GetObjectItem(...)->valuestring 在免费版上会产生空指针。 */
static int wea_parse(struct wea_buf *resp, char *text, rt_size_t text_size, int *temp)
{
    cJSON *root, *results, *first, *now, *item;
    int    rc = -1;

    root = cJSON_Parse(resp->data);
    if (root == RT_NULL)
    {
        rt_kprintf("[weather] json parse failed\n");
        return -1;
    }

    results = cJSON_GetObjectItem(root, "results");
    first   = cJSON_GetArrayItem(results, 0);
    if (first == RT_NULL)
    {
        /* 200 但没有 results，通常是 location 写法不被识别。
         * 把 body 头部打出来，否则用户只能看到屏上一直是 TBD。 */
        rt_kprintf("[weather] no results: %.120s\n", resp->data);
        cJSON_Delete(root);
        return -1;
    }

    now = cJSON_GetObjectItem(first, "now");
    if (now == RT_NULL)
    {
        rt_kprintf("[weather] no now object\n");
        cJSON_Delete(root);
        return -1;
    }

    item = cJSON_GetObjectItem(now, "text");
    if (cJSON_IsString(item) && item->valuestring != RT_NULL)
    {
        rt_strncpy(text, item->valuestring, text_size - 1);
        text[text_size - 1] = '\0';
        rc = 0;
    }

    item = cJSON_GetObjectItem(now, "temperature");
    if (cJSON_IsString(item) && item->valuestring != RT_NULL)
    {
        *temp = atoi(item->valuestring);
    }

    cJSON_Delete(root);

    /* text 字段是必需的：缺少它则该行读数没有意义。
     * 温度可以缺失（仅显示现象文字），因此不参与成败判定，
     * 否则免费版一旦不返回 temperature，整条天气信息都不会显示。
     * 注意不能因为 temperature 存在就将 rc 置为 0，否则 text 为空时
     * 会用空串覆盖上一次的有效数据。 */
    if (rc != 0)
        rt_kprintf("[weather] no 'text' field in response\n");

    return rc;
}

/* ---------- 一次完整拉取 ---------- */

static int wea_sync_once(void)
{
    char          url[320];
    struct wea_buf resp;
    char          text[sizeof(((app_data_t *)0)->weather_text)];
    int           temp = -1000;
    int           status;

    if (wea_build_url(url, sizeof(url)) != 0)
    {
        /* host / path / key 有一项为空。不要静默返回：用户会以为
         * 线程没跑，实际是配置不全。 */
        rt_kprintf("[weather] config incomplete (host/path/key), skip\n");
        return -1;
    }

    /* 日志中不得出现完整 URL，因为它包含 key（rt_kprintf 为明文串口输出）。 */
    rt_kprintf("[weather] fetch %s%s\n",
               app_cfg_get(APP_CFG_KEY_WEA_HOST),
               app_cfg_get(APP_CFG_KEY_WEA_PATH));

    status = wea_get_collect(url, &resp);
    if (status != 200)
    {
        if (resp.data != RT_NULL)
            rt_free(resp.data);
        return (status > 0) ? status : -1;
    }

    text[0] = '\0';
    if (wea_parse(&resp, text, sizeof(text), &temp) != 0)
    {
        rt_free(resp.data);
        return -3;
    }

    rt_free(resp.data);

    /* 温度与天气现象拼成一行显示在 Weather 那一格：
     * 免费版没有湿度，单独显示温度会与旁边的 Humidity 读数混淆。
     * text 缓冲取自 weather_text 的实际长度，拼出来的行一定装得下
     * （rt_snprintf 会截断，不会溢出）。 */
    if (temp != -1000)
    {
        char line[sizeof(text)];

        rt_snprintf(line, sizeof(line), "%s %dC", text, temp);
        rt_strncpy(text, line, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
    }

    app_data_set_weather(text);

    rt_kprintf("[weather] ok: %s\n", text);
    return 0;
}

/* ---------- 线程 ---------- */

static void wea_entry(void *parameter)
{
    rt_uint32_t fails = 0;

    RT_UNUSED(parameter);

    while (1)
    {
        if (!wea_net_ready())
        {
            rt_thread_mdelay(3000);
            continue;
        }

        /* key 没配就别发请求：心知会返回 AP010003，一轮轮重试
         * 既浪费流量又把串口刷满。当作"未配置"静默等待。 */
        if (app_cfg_get(APP_CFG_KEY_WEA_KEY)[0] == '\0')
        {
            rt_thread_mdelay(WEA_NO_KEY_MS);
            continue;
        }

        if (wea_sync_once() == 0)
        {
            fails = 0;
            rt_thread_mdelay(WEA_PERIOD_MS);
        }
        else
        {
            rt_uint32_t i = (fails < WEA_RETRY_N) ? fails : (WEA_RETRY_N - 1);

            if (fails < 255)
                fails++;
            rt_thread_mdelay(wea_retry_ms[i]);
        }
    }
}

static int weather_start(void)
{
    rt_thread_t tid;

    tid = rt_thread_create("weather", wea_entry, RT_NULL,
                           WEA_THREAD_STACK, WEA_THREAD_PRIO, 10);
    if (tid == RT_NULL)
    {
        rt_kprintf("[weather] thread create failed\n");
        return -RT_ERROR;
    }
    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(weather_start);

#endif /* PKG_USING_WEBCLIENT && PKG_USING_CJSON && RT_USING_NETDEV */
