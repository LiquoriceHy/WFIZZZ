/*
 * maimemo.c - 墨墨背单词开放 API 拉取线程
 *
 * 低优先级线程每 MEMO_PERIOD_MS 拉取一次今日学习数据并写入 g_app.memo，
 * UI（ui_memo.c）用 lv_timer 每秒读取刷新，沿用 app_data 与 wifi_auto 的
 * "线程写 volatile，UI 拉取"模式。不在 LVGL 线程中执行 TLS 请求，因为
 * LVGL 线程栈仅 24KB，而 mbedTLS 的 in/out 记录缓冲各 16KB，必然栈溢出。
 *
 * 使用的接口（官方标注为"公测"，需在墨墨 App 中开启"自动同步"，且当日
 * 打开过 App，否则 total 不准确或返回 study_data_not_found）：
 *   POST /open/api/v1/memo/study/get_study_progress  对应 data.progress.{finished,total}
 *   POST /open/api/v1/memo/study/get_today_items     对应 data.today_items[].first_response
 * 路径存在两套写法（官方规范带 /memo，官方 CLI 不带，实测均返回 200），
 * 先使用规范路径，遇到 404 则切换至另一套并记住。
 *
 * 三色条没有聚合接口，只能拉取今日已完成单词列表后在本地统计 first_response
 * （官方文档明确要求客户端自行过滤与统计）。响应可达 184KB（1000 词约
 * 184B/词），因此不做 cJSON 全量解析（解析树规模会是文本的数倍），
 * 而是直接在缓冲上按带引号的完整关键字计数：关键字在响应中每词恰好出现
 * 一次，且 FAMILIAR、VAGUE、FORGET、WELL_FAMILIAR 前缀互不包含，不会误计。
 */
#include <rtthread.h>
#include <rtdevice.h>

#if defined(PKG_USING_WEBCLIENT) && defined(PKG_USING_CJSON) && defined(RT_USING_NETDEV)

#include <lwip/ip_addr.h>   /* ip_addr_t / ip_addr_isany，须先于 netdev.h */
#include <netdev.h>

#include <webclient.h>
#include <cJSON.h>
#include <sys/time.h>       /* settimeofday/struct timeval（无 LIBC 配置也可用，同 app_data.c） */

#include "app_cfg.h"
#include "app_data.h"

extern rt_err_t get_timestamp(time_t *timestamp);   /* rtc.h 的软 RTC 读取，同 app_data.c */

/* ---------- RTC 校时（2026-09-29）----------
 * 墨墨 HTTP 响应的 Date 头是全系统唯一的网络时间源：解析 RFC7231
 * IMF-fixdate（"Tue, 29 Sep 2026 15:30:45 GMT"，定长定位，不依赖
 * libc 的 sscanf/strptime），算出 UTC epoch 后 settimeofday 写软 RTC
 * （= 系统时钟，RT_USING_SOFT_RTC）。偏差 <60s 不扰动。 */
static time_t mm_days_from_civil(int y, int m, int d)
{
    long era;
    unsigned int yoe, doy, doe;

    y -= (m <= 2);
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned int)(y - era * 400);
    doy = (unsigned int)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return (time_t)((long)era * 146097L + (long)doe - 719468L);
}

static void mm_rtc_sync_from_header(struct webclient_session *session)
{
    static const char mon12[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date;
    int mon_i, day, year, hh, mi, ss;
    time_t ts, now = 0, diff;
    struct timeval tv;
    extern rt_err_t get_timestamp(time_t *timestamp);

    date = webclient_header_fields_get(session, "Date");
    if (date == RT_NULL)
        return;

    /* IMF-fixdate: "Tue, 29 Sep 2026 15:30:45 GMT"（定长定位） */
    if (rt_strlen(date) < 25)
        return;
    day = (date[5] - '0') * 10 + (date[6] - '0');
    for (mon_i = 0; mon_i < 12; mon_i++)
    {
        if (rt_strncmp(&date[8], &mon12[mon_i * 3], 3) == 0)
            break;
    }
    if (mon_i >= 12)
        return;
    year = (date[12] - '0') * 1000 + (date[13] - '0') * 100 +
           (date[14] - '0') * 10 + (date[15] - '0');
    hh = (date[17] - '0') * 10 + (date[18] - '0');
    mi = (date[20] - '0') * 10 + (date[21] - '0');
    ss = (date[23] - '0') * 10 + (date[24] - '0');

    if (day < 1 || day > 31 || year < 2020 || year > 2099 ||
        hh > 23 || mi > 59 || ss > 59)
        return;

    ts = mm_days_from_civil(year, mon_i + 1, day) * 86400 +
         hh * 3600 + mi * 60 + ss;

    /* 偏差 <60s 不扰动系统时钟 */
    if (get_timestamp(&now) == RT_EOK && now > 0)
    {
        diff = (ts > now) ? (ts - now) : (now - ts);
        if (diff < 60)
            return;
    }

    tv.tv_sec = ts;
    tv.tv_usec = 0;
    if (settimeofday(&tv, RT_NULL) == 0)
        rt_kprintf("[maimemo] rtc synced from Date header\n");
}

#define MM_THREAD_STACK  12288   /* mbedTLS RSA 验签在栈上分配多个 mbedtls_mpi（各 1KB 以上）。
                                  * 2026-10-26 真机 list_thread 实测峰值为 16%（约 2.6KB），
                                  * 由 16KB 降至 12KB 仍保留 4.5 倍余量；TLS 握手为最坏情况，
                                  * 修改 mbedTLS 配置或更换证书链前须重新实测。 */
#define MM_THREAD_PRIO   25      /* 与 wifi_auto 同级，低于 LVGL(21)、音频(20)与 app_data(24) */

/* 拉取周期。墨墨频控为 10s 内 20 次、60s 内 40 次、5h 内 2000 次，
 * 每轮 2 个请求，10 分钟一轮即每小时 12 次，余量充足。
 * 2026-10-26 从 maimemo_cfg.h 迁移至此：该文件只剩凭据，
 * 而凭据已改由 SD 卡配置提供（见 app_cfg.h）。 */
#define MEMO_PERIOD_MS   (10 * 60 * 1000)

/* 没配 token 时的轮询间隔。比正常周期短，用户刚在网页填完能较快生效。 */
#define MM_NO_TOKEN_MS   (30 * 1000)

/* today_items 官方上限 1000 词，实测 ~184B/词 ≈ 184KB，封顶 200KB */
#define MM_RESP_INIT     (16 * 1024)
#define MM_RESP_MAX      (200 * 1024)

/* 失败重试退避序列，之后维持最后一档 */
#define MM_RETRY_N       4
static const rt_uint32_t mm_retry_ms[MM_RETRY_N] = {30000, 60000, 120000, 300000};

/* 路径两套写法：0=官方规范（带 /memo），1=官方 CLI（不带） */
static const char *const mm_path_progress[2] =
{
    "/open/api/v1/memo/study/get_study_progress",
    "/open/api/v1/study/get_study_progress",
};
static const char *const mm_path_items[2] =
{
    "/open/api/v1/memo/study/get_today_items",
    "/open/api/v1/study/get_today_items",
};
static rt_uint8_t mm_path_idx = 0;   /* 记住当前可用的写法，跨周期复用 */

/* first_response 枚举 → 三色桶（熟知归入认识；CANCEL_* 与空值不计） */
enum { MM_KNOWN = 0, MM_FUZZY = 1, MM_FORGET = 2 };
static const struct
{
    const char *key;
    rt_uint8_t  bucket;
} mm_resp_keys[] =
{
    { "\"first_response\":\"FAMILIAR\"",      MM_KNOWN },
    { "\"first_response\":\"VAGUE\"",         MM_FUZZY },
    { "\"first_response\":\"FORGET\"",        MM_FORGET },
    { "\"first_response\":\"WELL_FAMILIAR\"", MM_KNOWN },
};

/* ---------- 联网判定（与 app_data.c 同款） ---------- */

static rt_bool_t mm_net_ready(void)
{
    struct netdev *nd = netdev_get_by_name("w0");   /* wlan_lwip.c 注册名 */

    if (nd == RT_NULL)
        nd = netdev_default;

    return (nd != RT_NULL && netdev_is_up(nd) &&
            netdev_is_link_up(nd) && !ip_addr_isany(&nd->ip_addr));
}

/* ---------- HTTP ---------- */

struct mm_buf
{
    char     *data;
    rt_size_t len;
};

/* POST JSON 并把响应体收进几何增长缓冲（调用方 rt_free(out->data)）。
 * 返回 HTTP 状态码；<0 本地失败；-2 响应超 MM_RESP_MAX。 */
static int mm_post_collect(const char *path, const char *body, struct mm_buf *out)
{
    struct webclient_session *session;
    char url[96];
    int status;
    rt_size_t cap = 0;

    out->data = RT_NULL;
    out->len = 0;

    /* 地址与令牌来自运行时配置（SD 卡 /sdcard/wfizzz/cfg.json）。
     * 2026-10-26 起不再使用 maimemo_cfg.h 的宏：凭据编译进固件既无法修改
     * 也无法删除，而工作区为 git 仓库，明文 token 入库即等同于公开。
     * 默认值现仅保留在 app_cfg.c 中，且 token 默认为空。 */
    rt_snprintf(url, sizeof(url), "https://%s%s",
                app_cfg_get(APP_CFG_KEY_MEMO_HOST), path);

    session = webclient_session_create(WEBCLIENT_HEADER_BUFSZ);
    if (session == RT_NULL)
        return -1;

    /* webclient_send_header 只自动补 Host/User-Agent/Accept（webclient.c:686-702），
     * Content-Length/Content-Type 必须自己加，否则 POST 体发不出去 */
    webclient_header_fields_add(session, "Authorization: Bearer %s\r\n",
                                app_cfg_get(APP_CFG_KEY_MEMO_TOKEN));
    webclient_header_fields_add(session, "Content-Type: application/json\r\n");
    webclient_header_fields_add(session, "Content-Length: %d\r\n", (int)rt_strlen(body));

    status = webclient_post(session, url, body, rt_strlen(body));
    if (status <= 0)
    {
        webclient_close(session);
        return (status < 0) ? status : -1;
    }

    mm_rtc_sync_from_header(session);   /* 响应头 Date → 软 RTC 校时（60s 阈值内不扰动） */

    while (1)
    {
        rt_size_t got;

        if (out->len + 256 > cap)
        {
            char *nb;
            rt_size_t ncap = cap ? cap * 2 : MM_RESP_INIT;

            if (ncap > MM_RESP_MAX)
                ncap = MM_RESP_MAX;
            if (ncap <= cap)                /* 已到上限，无法继续收 */
            {
                rt_kprintf("[maimemo] response over %dKB\n", (int)(MM_RESP_MAX / 1024));
                rt_free(out->data);
                out->data = RT_NULL;
                webclient_close(session);
                return -2;
            }
            nb = rt_realloc(out->data, ncap);
            if (nb == RT_NULL)
            {
                rt_free(out->data);
                out->data = RT_NULL;
                webclient_close(session);
                return -1;
            }
            out->data = nb;
            cap = ncap;
        }

        got = webclient_read(session, (unsigned char *)out->data + out->len,
                             cap - out->len - 1);
        if (got <= 0)
            break;
        out->len += got;
    }

    if (out->data != RT_NULL)
        out->data[out->len] = '\0';

    webclient_close(session);
    return status;
}

/* 规范路径 404 时切到官方 CLI 写法重试一次（实测两套均可用，此为保险） */
static int mm_post_auto(const char *const path[2], const char *body, struct mm_buf *out)
{
    int status = mm_post_collect(path[mm_path_idx], body, out);

    if (status == 404 && mm_path_idx == 0)
    {
        mm_path_idx = 1;
        rt_free(out->data);
        status = mm_post_collect(path[mm_path_idx], body, out);
    }
    return status;
}

/* ---------- 两个接口 ---------- */

/* 200 但 success:false 时按 errors[0].code 细分原因。不能将 valuestring
 * 带出函数，因为它在 cJSON_Delete 后即失效，必须在此处完成比较。 */
static rt_uint8_t mm_state_from_error(struct mm_buf *resp)
{
    cJSON *root = cJSON_Parse(resp->data);
    rt_uint8_t st = APP_DATA_MEMO_ERROR;

    if (root != RT_NULL)
    {
        cJSON *e0 = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "errors"), 0);
        cJSON *item = cJSON_IsObject(e0) ? cJSON_GetObjectItem(e0, "code") : RT_NULL;

        if (cJSON_IsString(item))
        {
            if (rt_strcmp(item->valuestring, "common_unauthorized") == 0)
                st = APP_DATA_MEMO_TOKEN;
            else if (rt_strcmp(item->valuestring, "study_data_not_found") == 0)
                st = APP_DATA_MEMO_SYNC;
        }
        cJSON_Delete(root);
    }
    return st;
}

/* 今日进度。成功填 m->done/total 返回 OK，否则返回对应错误态 */
static rt_uint8_t mm_fetch_progress(app_data_memo_t *m)
{
    struct mm_buf resp;
    int status;
    rt_uint8_t st = APP_DATA_MEMO_ERROR;

    status = mm_post_auto(mm_path_progress, "{}", &resp);
    if (status == 200)
    {
        cJSON *root = cJSON_Parse(resp.data);

        if (root != RT_NULL)
        {
            cJSON *progress = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"),
                                                  "progress");

            if (cJSON_IsObject(progress))
            {
                cJSON *fin = cJSON_GetObjectItem(progress, "finished");
                cJSON *tot = cJSON_GetObjectItem(progress, "total");

                if (cJSON_IsNumber(fin) && cJSON_IsNumber(tot))
                {
                    m->done  = (rt_uint16_t)fin->valueint;
                    m->total = (rt_uint16_t)tot->valueint;
                    st = APP_DATA_MEMO_OK;
                }
            }

            /* 200 但 success:false：按 errors[0].code 细分原因 */
            if (st != APP_DATA_MEMO_OK)
                st = mm_state_from_error(&resp);
            cJSON_Delete(root);
        }
    }
    else if (status == 401 || status == 403)
    {
        st = APP_DATA_MEMO_TOKEN;
    }

    rt_free(resp.data);
    return st;
}

/* 今日已完成单词的反馈分布（关键字计数）。成功填 m->known/fuzzy/forgot */
static rt_uint8_t mm_fetch_items(app_data_memo_t *m)
{
    static const char body[] = "{\"is_finished\":true,\"limit\":1000}";
    struct mm_buf resp;
    int status;
    rt_size_t i;
    rt_uint32_t cnt[3] = {0, 0, 0};
    rt_uint8_t st = APP_DATA_MEMO_ERROR;

    status = mm_post_auto(mm_path_items, body, &resp);
    if (status == 200 && resp.data != RT_NULL)
    {
        st = APP_DATA_MEMO_OK;
        for (i = 0; i < sizeof(mm_resp_keys) / sizeof(mm_resp_keys[0]); i++)
        {
            const char *p = resp.data;
            const rt_size_t klen = rt_strlen(mm_resp_keys[i].key);

            while ((p = rt_strstr(p, mm_resp_keys[i].key)) != RT_NULL)
            {
                cnt[mm_resp_keys[i].bucket]++;
                p += klen;
            }
        }
    }
    else if (status == 401 || status == 403)
    {
        st = APP_DATA_MEMO_TOKEN;
    }

    m->known  = (rt_uint16_t)cnt[MM_KNOWN];
    m->fuzzy  = (rt_uint16_t)cnt[MM_FUZZY];
    m->forgot = (rt_uint16_t)cnt[MM_FORGET];

    rt_free(resp.data);
    return st;
}

/* ---------- 同步一轮 + 线程 ---------- */

static void mm_sync_once(void)
{
    app_data_memo_t m;
    rt_uint8_t st;

    rt_memset(&m, 0, sizeof(m));

    st = mm_fetch_progress(&m);
    if (st == APP_DATA_MEMO_OK)
        st = mm_fetch_items(&m);    /* 进度成功但列表失败：整轮按失败处理，不显示半套数据 */

    if (st != APP_DATA_MEMO_OK)
        rt_memset(&m, 0, sizeof(m));
    m.state = st;
    app_data_set_memo(&m);

    rt_kprintf("[maimemo] %u/%u known=%u fuzzy=%u forgot=%u state=%u\n",
               (unsigned int)m.done, (unsigned int)m.total,
               (unsigned int)m.known, (unsigned int)m.fuzzy,
               (unsigned int)m.forgot, (unsigned int)m.state);
}

static void mm_entry(void *parameter)
{
    rt_uint8_t fails = 0;

    RT_UNUSED(parameter);

    while (1)
    {
        if (!mm_net_ready())
        {
            app_data_memo_t m;

            rt_memset(&m, 0, sizeof(m));
            m.state = APP_DATA_MEMO_OFFLINE;
            app_data_set_memo(&m);

            rt_thread_mdelay(3000);
            continue;
        }

        /* 未配置 token 时不发送请求：墨墨接口未授权时返回 401，
         * 反复重试既消耗流量又产生大量日志。此处视为"未配置"并静默等待，
         * 用户在网页填写 token 后下一轮自然生效，无需重启。 */
        if (app_cfg_get(APP_CFG_KEY_MEMO_TOKEN)[0] == '\0')
        {
            app_data_memo_t m;

            rt_memset(&m, 0, sizeof(m));
            m.state = APP_DATA_MEMO_OFFLINE;
            app_data_set_memo(&m);

            rt_thread_mdelay(MM_NO_TOKEN_MS);
            continue;
        }

        mm_sync_once();

        if (g_app.memo.state == APP_DATA_MEMO_OK)
        {
            fails = 0;
            rt_thread_mdelay(MEMO_PERIOD_MS);
        }
        else
        {
            /* 越界取最后一档：fails 无符号，先判后取 */
            rt_uint32_t wait = mm_retry_ms[(fails < MM_RETRY_N) ? fails : (MM_RETRY_N - 1)];

            if (fails < 255)
                fails++;
            rt_thread_mdelay(wait);
        }
    }
}

static int maimemo_start(void)
{
    rt_thread_t tid;

    tid = rt_thread_create("maimemo", mm_entry, RT_NULL,
                           MM_THREAD_STACK, MM_THREAD_PRIO, 10);
    if (tid == RT_NULL)
    {
        rt_kprintf("[maimemo] thread create failed\n");
        return -RT_ERROR;
    }
    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(maimemo_start);

#endif /* PKG_USING_WEBCLIENT && PKG_USING_CJSON && RT_USING_NETDEV */
