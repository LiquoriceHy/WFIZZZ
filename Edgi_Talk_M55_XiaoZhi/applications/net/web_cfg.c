/*
 * web_cfg.c - 板载配置网页的实现
 *
 * 组成：
 *   1) webnet 文件服务：webnet_set_root(WEB_CFG_ROOT) + webnet_init()。
 *      SD 上的 www/ 目录有资源就用资源，没有就由 CGI 输出内嵌兜底页。
 *   2) CGI：/cgi-bin/ 下六个端点
 *        status   当前配网/存储状态（JSON）
 *        get      读全部配置（敏感字段掩码）
 *        set      改一个或多个字段（urlencoded）
 *        scan     WiFi 扫描（JSON 数组）
 *        wifi     发起 STA 连接 / 启 SoftAP
 *        reboot   重启
 *   3) 配网状态机线程：有凭据就 STA 连接，失败或无凭据就起 SoftAP。
 *
 * 为什么内嵌兜底页：
 *   SD 卡可能未插入，而首次配网恰恰最可能处于未插卡状态（板子刚到手）。
 *   内嵌页在内存中约 2.6KB（-Os 下即为源码长度，未使用压缩），
 *   以该代价换取"始终可以配网"这一关键能力，是合理的。
 *   页面主体（HTML/CSS/JS）放在 SD 上，以便修改样式时无需重新烧录固件。
 *
 * 为什么 CGI 不使用 JSON 请求体：
 *   webnet 仅在 Content-Type 为 application/x-www-form-urlencoded 时
 *   解析 POST body（wn_request.c:514）。发送 JSON 时 query 不会被填充，
 *   参数全部为 NULL。因此前端统一使用 urlencoded。
 *
 * 内存：
 *   每个 webnet 会话由 rt_malloc 分配 4.1KB（结构体内含 4KB buffer，
 *   wn_session.h:40），而主堆余量仅约 40KB。浏览器一次打开 6 个连接即
 *   占用 25KB。因此页面刻意只保留 3 个文件（index.html 内联 css/js），
 *   且前端串行发送请求。若日后主堆更为紧张，优先降低 WEBNET_CONN_MAX。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>      /* isxdigit：param() 的百分号解码 */
#include <stdlib.h>     /* strtol  */

#include <webnet.h>
#include <wn_module.h>
#include <wn_request.h>
#include <wn_session.h>

/* netdev_ipaddr.h 才是 inet_ntoa / ip_addr_isany 的正确来源：
 * 它按"启用了 lwIP 还是 SAL 还是 IPv6"切换实现（netdev_ipaddr.h:257-303），
 * 直接包 lwip/ip_addr.h 会拿错宏或者在 SAL 配置下根本找不到定义。
 * 它自己会 #include <rtconfig.h> 并转发到对应后端。 */
#include <netdev_ipaddr.h>
#include <netdev.h>
#include <wlan_mgnt.h>

#include "app_cfg.h"
#include "web_cfg.h"
#include "snore_rt.h"

#define DBG_TAG "app.webcfg"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* ===================== 状态 ===================== */

enum
{
    WCFG_ST_IDLE = 0,       /* 未配网，等用户填 */
    WCFG_ST_CONNECTING,     /* STA 连接中 */
    WCFG_ST_CONNECTED,      /* STA 已连 */
    WCFG_ST_AP,             /* SoftAP 已启动 */
};

static volatile int s_state = WCFG_ST_IDLE;
static volatile rt_bool_t s_webnet_up = RT_FALSE;
static volatile rt_bool_t s_ap_active = RT_FALSE;
static volatile rt_bool_t s_need_reconnect = RT_FALSE;
static struct rt_mutex s_wifi_lock;
static rt_bool_t s_wifi_lock_ok = RT_FALSE;
/* s_wifi_lock / s_scan_req 是否已 init 过。静态内核对象不得 init 两次，
 * 否则命中 rt_object_init 的 `obj != object` 断言。 */
static rt_bool_t s_lock_inited = RT_FALSE;
static rt_bool_t s_scan_inited = RT_FALSE;

int web_cfg_get_state(void)
{
    return s_state;
}

rt_bool_t web_cfg_is_serving(void)
{
    return s_webnet_up && (s_state == WCFG_ST_CONNECTED || s_state == WCFG_ST_AP);
}

/* ===================== 内嵌兜底页 ===================== */
/* 只有 SD 上没有 index.html 时才会被送到浏览器。
 * 保持极简：能改 WiFi + 能看状态就够，样式留给 SD 版。 */

static const char s_fallback_html[] =
"<!DOCTYPE html><html><head><meta charset=utf-8>"
"<meta name=viewport content=\"width=device-width,initial-scale=1\">"
"<title>WFIZZZ 配置（精简版）</title>"
"<style>"
"body{font-family:-apple-system,Segoe UI,sans-serif;margin:0;padding:16px;"
"background:#111;color:#eee;font-size:16px}"
"h1{font-size:20px;font-weight:600;margin:0 0 4px}"
"p.sub{color:#888;font-size:13px;margin:0 0 20px}"
"label{display:block;color:#aaa;font-size:13px;margin:14px 0 6px}"
"input,select{width:100%;box-sizing:border-box;padding:12px;font-size:16px;"
"border:1px solid #333;border-radius:8px;background:#1c1c1c;color:#eee}"
"button{width:100%;padding:14px;font-size:16px;font-weight:600;margin-top:20px;"
"border:0;border-radius:8px;background:#3b82f6;color:#fff}"
"button.sec{background:#333;margin-top:10px}"
"#msg{margin-top:16px;padding:12px;border-radius:8px;font-size:14px;"
"background:#1c1c1c;color:#9ca3af;display:none}"
"#msg.on{display:block}"
".ok{color:#34d399!important}.err{color:#f87171!important}"
"</style></head><body>"
"<h1>WFIZZZ 配置</h1>"
"<p class=sub>SD 卡上没有 www 页面，正在使用内置精简版。</p>"
"<label>WiFi 名称 (2.4GHz)</label>"
"<input id=s maxlength=32 placeholder=\"例如 MyHome-2.4G\">"
"<label>WiFi 密码</label>"
"<input id=p type=password maxlength=64 placeholder=\"留空=不改\">"
"<button onclick=save()>保存并连接</button>"
"<button class=sec onclick=location.href='/cgi-bin/get'>查看完整配置</button>"
"<div id=msg></div>"
"<script>"
"function M(t,c){var m=document.getElementById('msg');"
"m.className='on '+(c||'');m.textContent=t}"
"var q=location.search.match(/[?&]ssid=([^&]*)/);"
"if(q)document.getElementById('s').value=decodeURIComponent(q[1]);"
"function save(){var s=document.getElementById('s').value.trim();"
"var p=document.getElementById('p').value;if(!s){M('请填 WiFi 名称','err');return}"
"M('保存中…');"
/* 逐字段提交 + 最后触发连接。不能一次发 ssid+pass：webnet 2.0.3
 * 解析多参数时后面的参数会被截断（密码恰在第二位）。
 * 值编码两次，见 app.js 里 post() 的说明。 */
"function P(b){return fetch('/cgi-bin/set',{method:'POST',headers:"
"{'Content-Type':'application/x-www-form-urlencoded'},body:b})}"
"P('wifi_ssid='+encodeURIComponent(encodeURIComponent(s)))"
".then(function(){return p?P('wifi_pass='+encodeURIComponent(encodeURIComponent(p))):null})"
".then(function(){return fetch('/cgi-bin/wifi',{method:'POST',headers:"
"{'Content-Type':'application/x-www-form-urlencoded'},body:'go=1'})})"
".then(function(r){return r.text()}).then(function(t){M(t&&t.length?'已提交，正在连接…':'ok','ok')})"
".catch(function(e){M('请求失败: '+e,'err')})}"
"</script></body></html>";

/* ===================== 小工具 ===================== */

/* 百分号解码一层（就地）。
 *
 * 需要这一层的原因：webnet 2.0.3 的 _webnet_request_parse_query()
 * 在扫到 '=' 之后调用 urldecode() 就地解码 value，随后 ptr 继续往后
 * 扫描同一缓冲区。因此值中解码出的 '&' 会被当作参数分隔符而提前
 * 结束，解码出的 '=' 会被当作分隔符而使前半段丢失。实测结果：
 *     A%2FB -> "A/B"   正常
 *     A%26B -> "A"     '&' 之后被截断
 *     A%3DB -> "B"     '=' 之前被丢弃
 * 天气路径 "/data/2.5/weather?q=Beijing&units=metric" 正是受影响者，
 * 存入后只剩 "Beijing"。
 *
 * 修法（不改第三方包）：前端对每个值编码两次，webnet 解掉一层后只剩
 * 纯字母数字和 %XX，不会再触发上述截断；这里再把剩下的一层解掉。
 * 前端见 app.js 的 post()。 */
static void percent_decode_inplace(char *s)
{
    char *out = s;

    while (*s)
    {
        /* s[1] / s[2] 都要先确认非 '\0' 才能读：
         * 形如 "%2" 结尾的残缺序列不能让 s[2] 越过字符串末尾。 */
        if (*s == '%' && s[1] != '\0' && s[2] != '\0' &&
            isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2]))
        {
            char hex[3];

            hex[0] = s[1];
            hex[1] = s[2];
            hex[2] = '\0';
            *out++ = (char)strtol(hex, RT_NULL, 16);
            s += 3;
        }
        else
        {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

/* 参数解码缓冲：一次 CGI 调用里可能有十几个参数，
 * 逐个放进轮转槽位，避免每次都 rt_malloc。
 * 64 字节够放 host/path/model；token 类字段最长的 memo_token 是 64，
 * 加上解码后不会变长，96 字节留足余量。 */
#define PARAM_SLOTS     8
#define PARAM_SLOT_LEN  128

/* 取表单/查询参数并解码一层。GET 走 query，POST urlencoded 同样填入
 * query，因此本函数对两种方法通用。
 *
 * 返回值指向轮转缓冲，下一次调用本函数即失效。
 * 调用方若需跨多次 param() 保留，必须自行拷贝。 */
static const char *param(struct webnet_session *session, const char *name)
{
    static char slots[PARAM_SLOTS][PARAM_SLOT_LEN];
    static int  slot_idx = 0;

    const char *raw;
    char *dst;
    rt_size_t n;

    if (session == RT_NULL || session->request == RT_NULL) return RT_NULL;

    raw = webnet_request_get_query(session->request, (char *)name);
    if (raw == RT_NULL) return RT_NULL;

    dst = slots[slot_idx];
    slot_idx = (slot_idx + 1) % PARAM_SLOTS;

    n = rt_strlen(raw);
    if (n >= PARAM_SLOT_LEN) n = PARAM_SLOT_LEN - 1;
    rt_memcpy(dst, raw, n);
    dst[n] = '\0';

    percent_decode_inplace(dst);

    return dst;
}

static void send_json(struct webnet_session *session, const char *body)
{
    int len = (int)rt_strlen(body);

    webnet_session_set_header(session, "application/json", 200, "OK", len);
    webnet_session_write(session, (const rt_uint8_t *)body, len);
}

/* 百分号转义：将值安全放入 JSON 字符串。凭据中的 " 与 \ 必须转义，
 * 否则前端 JSON.parse 会失败。 */
static void json_put_escaped(struct webnet_session *session, const char *s)
{
    char buf[8];

    if (s == RT_NULL) s = "";

    for (; *s; s++)
    {
        unsigned char c = (unsigned char)*s;

        if (c == '"' || c == '\\')
        {
            buf[0] = '\\';
            buf[1] = (char)c;
            webnet_session_write(session, (const rt_uint8_t *)buf, 2);
        }
        else if (c == '\n') webnet_session_write(session, (const rt_uint8_t *)"\\n", 2);
        else if (c == '\r') webnet_session_write(session, (const rt_uint8_t *)"\\r", 2);
        else if (c == '\t') webnet_session_write(session, (const rt_uint8_t *)"\\t", 2);
        else if (c < 0x20)
        {
            rt_snprintf(buf, sizeof(buf), "\\u%04x", c);
            webnet_session_write(session, (const rt_uint8_t *)buf, (rt_size_t)rt_strlen(buf));
        }
        else
        {
            webnet_session_write(session, &c, 1);
        }
    }
}

/* 敏感字段的网页回显：只给出掩码加后 4 位。
 * 空值保持空，页面据此区分"未设置"与"已设置但不显示"。 */
static void mask_secret(const char *v, char *out, rt_size_t cap)
{
    rt_size_t n = rt_strlen(v);

    if (n == 0) { out[0] = '\0'; return; }

    if (n <= 4)
    {
        /* 太短就整体掩掉，否则等于没掩 */
        rt_strncpy(out, "****", cap - 1);
        out[cap - 1] = '\0';
        return;
    }

    rt_snprintf(out, cap, "****%s", v + n - 4);
}

/* ===================== CGI: status ===================== */

static void cgi_status(struct webnet_session *session)
{
    char body[256];
    const char *ip = "-";
    struct netdev *nd;
    int n;

    if (s_state == WCFG_ST_AP)
    {
        ip = DHCPD_SERVER_IP;   /* 192.168.169.1 */
    }
    else
    {
        nd = netdev_get_by_name("w0");
        if (nd == RT_NULL) nd = netdev_default;
        if (nd != RT_NULL && !ip_addr_isany(&nd->ip_addr))
            ip = inet_ntoa(nd->ip_addr);
    }

    n = rt_snprintf(body, sizeof(body),
                    "{\"state\":%d,\"ip\":\"%s\",\"persist\":%s,"
                    "\"dirty\":%s,\"ssid\":\"",
                    s_state, ip,
                    app_cfg_persist_ok() ? "true" : "false",
                    g_app_cfg.dirty ? "true" : "false");
    webnet_session_set_header(session, "application/json", 200, "OK", -1);
    /* Content-Length 未知时 webnet 会发 Connection: close，
     * 所以这里可以安全地分块写。 */
    webnet_session_write(session, (const rt_uint8_t *)body, (rt_size_t)n);
    json_put_escaped(session, g_app_cfg.wifi_ssid);
    webnet_session_write(session, (const rt_uint8_t *)"\",\"rssi\":", 9);

    /* rssi 只在 STA 已连时有意义 */
    n = rt_snprintf(body, sizeof(body), "%d}",
                    (s_state == WCFG_ST_CONNECTED) ? rt_wlan_get_rssi() : 0);
    webnet_session_write(session, (const rt_uint8_t *)body, (rt_size_t)n);
}

/* ===================== CGI: get ===================== */

/* 字段表位于 app_cfg.c 内部，此处只按 key 遍历，顺序即页面顺序，
 * 因此这两个数组必须同步维护；新增字段时两边都要修改。 */
static const char *const s_cfg_keys[] =
{
    APP_CFG_KEY_WIFI_SSID, APP_CFG_KEY_WIFI_PASS,
    APP_CFG_KEY_XZ_HOST,   APP_CFG_KEY_XZ_PATH,  APP_CFG_KEY_XZ_TOKEN,
    APP_CFG_KEY_XZ_PORT,   APP_CFG_KEY_XZ_OTA_PORT, APP_CFG_KEY_XZ_SSL,
    APP_CFG_KEY_MEMO_HOST, APP_CFG_KEY_MEMO_TOKEN,
    APP_CFG_KEY_WEA_HOST,  APP_CFG_KEY_WEA_PATH, APP_CFG_KEY_WEA_KEY,
    APP_CFG_KEY_WEA_CITY,
    APP_CFG_KEY_LLM_HOST,  APP_CFG_KEY_LLM_PATH, APP_CFG_KEY_LLM_KEY,
    APP_CFG_KEY_LLM_MODEL,
    RT_NULL
};

static void cgi_get(struct webnet_session *session)
{
    int i;
    rt_bool_t first = RT_TRUE;

    webnet_session_set_header(session, "application/json", 200, "OK", -1);
    webnet_session_write(session, (const rt_uint8_t *)"{", 1);

    for (i = 0; s_cfg_keys[i] != RT_NULL; i++)
    {
        const char *key = s_cfg_keys[i];
        const char *val = app_cfg_get(key);
        char masked[APP_CFG_KEY_LEN];

        if (val == RT_NULL) continue;

        if (!first) webnet_session_write(session, (const rt_uint8_t *)",", 1);
        first = RT_FALSE;

        webnet_session_write(session, (const rt_uint8_t *)"\"", 1);
        json_put_escaped(session, key);
        webnet_session_write(session, (const rt_uint8_t *)"\":\"", 3);

        if (app_cfg_is_secret(key))
        {
            mask_secret(val, masked, sizeof(masked));
            json_put_escaped(session, masked);
        }
        else
        {
            json_put_escaped(session, val);
        }

        webnet_session_write(session, (const rt_uint8_t *)"\"", 1);
    }

    webnet_session_write(session, (const rt_uint8_t *)"}", 1);
}

/* ===================== CGI: set ===================== */

/* 提交规则：
 *   - 参数名即 cfg key，一次可以提交多个字段。
 *   - 值为空表示不修改。这样用户只改一项时无需重新抄写其它密钥
 *     （页面回显的是掩码，用户无法抄出原值）。
 *   - 未知 key 忽略并计数，前端可看到"跳过了 N 项"。
 *   - 支持特殊 key "clear:<name>" 将字段清空。 */
static void cgi_set(struct webnet_session *session)
{
    char body[192];
    int i, ok = 0, skipped = 0, cleared = 0;

    for (i = 0; s_cfg_keys[i] != RT_NULL; i++)
    {
        const char *v = param(session, s_cfg_keys[i]);

        if (v == RT_NULL || v[0] == '\0') continue;   /* 空 = 不改 */

        if (app_cfg_set(s_cfg_keys[i], v) == 0) ok++;
        else skipped++;
    }

    /* "clear:key" 形式的显式清空 */
    for (i = 0; s_cfg_keys[i] != RT_NULL; i++)
    {
        char ck[48];
        const char *v;

        rt_snprintf(ck, sizeof(ck), "clear:%s", s_cfg_keys[i]);
        v = param(session, ck);
        if (v != RT_NULL && v[0] != '\0')
        {
            if (app_cfg_set(s_cfg_keys[i], "") == 0) cleared++;
        }
    }

    rt_snprintf(body, sizeof(body),
                "{\"ok\":%d,\"skipped\":%d,\"cleared\":%d,\"persist\":%s}",
                ok, skipped, cleared,
                app_cfg_persist_ok() ? "true" : "false");
    send_json(session, body);
}

/* ===================== CGI: scan ===================== */

/* ===================== 扫描（UI 与 CGI 共用） =====================
 *
 * 一次扫描需要 2-6 秒。HTTP 请求不能等待（浏览器会超时），LVGL 线程
 * 更不能等待（整屏卡死、触摸无响应）。因此统一改为：
 *   1) 扫描在独立的常驻线程中执行，结果写入静态快照；
 *   2) UI 与 CGI 只读快照，不阻塞。
 *
 * 采用常驻线程而非每次 create 的原因：本工程主堆仅剩约 40KB，反复
 * create/delete 2KB 栈会产生碎片；且扫描本身为低频操作，常驻一个
 * 1.5KB 栈的线程（平时阻塞在信号量上）代价更小、更可预测。
 */

static web_cfg_ap_t   s_scan_ap[WEB_CFG_SCAN_MAX];
static volatile int   s_scan_cnt;        /* 已收条目数 */
static volatile int   s_scan_state = WEB_CFG_SCAN_IDLE;
static volatile rt_tick_t s_scan_start_tick;   /* RUNNING 起始时刻，用于超时自愈 */

/* RUNNING 卡死保护：扫描线程正常路径一定会在 20 秒内将状态改为
 * DONE 或 FAIL；但若线程被意外挂起，或状态机被其它路径干扰，状态会
 * 永久停留在 RUNNING，之后每次 scan_begin 都被拦截为 EBUSY，功能完全
 * 失效。因此入口处设置基于滴答的软超时：超过 30 秒的 RUNNING 视为
 * 失效，自动退回 IDLE 允许重新发起。 */
#define WCFG_SCAN_STUCK_MS   30000

static struct rt_semaphore s_scan_req;   /* UI/CGI 释放一次 = 请求扫描 */
static rt_bool_t s_scan_req_ok = RT_FALSE;
static volatile rt_bool_t s_scan_done_flag;

/* 扫描结果回调。运行在 WHD 事件线程上下文，只做纯内存拷贝，不执行 IO。 */
static void scan_ap_cb(int event, struct rt_wlan_buff *buff, void *parameter)
{
    struct rt_wlan_info *info;
    int idx;
    int i;

    RT_UNUSED(parameter);

    if (event != RT_WLAN_EVT_SCAN_REPORT || buff == RT_NULL) return;

    info = (struct rt_wlan_info *)buff->data;
    if (info == RT_NULL || info->ssid.len == 0) return;

    /* 去重：同一 SSID 在多信道或多 BSSID 上会重复上报，UI 列表只需一条。
     * 保留信号更强的条目，并按原位置就地更新，避免列表跳动。 */
    for (i = 0; i < s_scan_cnt; i++)
    {
        if (rt_strlen(s_scan_ap[i].ssid) == info->ssid.len &&
            rt_memcmp(s_scan_ap[i].ssid, info->ssid.val, info->ssid.len) == 0)
        {
            if (info->rssi > s_scan_ap[i].rssi)
            {
                s_scan_ap[i].rssi    = info->rssi;
                s_scan_ap[i].channel = info->channel;
                s_scan_ap[i].secure  = (info->security != SECURITY_OPEN);
            }
            return;
        }
    }

    idx = s_scan_cnt;
    if (idx >= WEB_CFG_SCAN_MAX) return;      /* 满了就丢，不覆盖已有 */

    rt_memset(&s_scan_ap[idx], 0, sizeof(s_scan_ap[idx]));
    rt_memcpy(s_scan_ap[idx].ssid, info->ssid.val,
              (info->ssid.len < WEB_CFG_SSID_MAX) ? info->ssid.len : WEB_CFG_SSID_MAX - 1);
    s_scan_ap[idx].ssid[WEB_CFG_SSID_MAX - 1] = '\0';
    s_scan_ap[idx].rssi    = info->rssi;
    s_scan_ap[idx].channel = info->channel;
    s_scan_ap[idx].secure  = (info->security != SECURITY_OPEN);

    s_scan_cnt = idx + 1;
}

static void scan_done_cb(int event, struct rt_wlan_buff *buff, void *parameter)
{
    RT_UNUSED(event);
    RT_UNUSED(buff);
    RT_UNUSED(parameter);
    s_scan_done_flag = RT_TRUE;
}

/* 扫描线程：平时挂在 s_scan_req 上，被唤醒才干活。 */
static void web_cfg_scan_thread(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        int waited = 0;
        rt_err_t err;

        /* 等一次扫描请求 */
        if (rt_sem_take(&s_scan_req, RT_WAITING_FOREVER) != RT_EOK)
            continue;

        s_scan_cnt = 0;
        s_scan_done_flag = RT_FALSE;
        /* 刷新起始时刻：web_cfg_scan_begin 里也设过，这里再设一次是为了
         * 覆盖"请求与线程真正开工之间有延迟"的情况，让软超时从实际
         * 开工时刻起算。 */
        s_scan_start_tick = rt_tick_get();
        s_scan_state = WEB_CFG_SCAN_RUNNING;

        /* 单槽注册：先注册再发起，扫完立刻注销，否则会挤掉框架
         * 自己的 SCAN_REPORT 处理链（rt_wlan_connect 内部要用）。 */
        rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_REPORT, scan_ap_cb, RT_NULL);
        rt_wlan_register_event_handler(RT_WLAN_EVT_SCAN_DONE, scan_done_cb, RT_NULL);

        err = rt_wlan_scan();
        if (err != RT_EOK)
        {
            LOG_W("scan start failed: %d", err);
            s_scan_state = WEB_CFG_SCAN_FAIL;
        }
        else
        {
            /* 20 秒覆盖 2.4GHz 全信道；超时也保留已扫到的条目 */
            while (waited < 20000 && !s_scan_done_flag)
            {
                rt_thread_mdelay(100);
                waited += 100;
            }
            if (!s_scan_done_flag) LOG_W("scan timeout after %d ms", waited);
            s_scan_state = WEB_CFG_SCAN_DONE;
            LOG_I("scan done: %d ap in %d ms", s_scan_cnt, waited);
        }

        rt_wlan_unregister_event_handler(RT_WLAN_EVT_SCAN_REPORT);
        rt_wlan_unregister_event_handler(RT_WLAN_EVT_SCAN_DONE);
    }
}

int web_cfg_scan_begin(void)
{
    if (!s_scan_req_ok) return -RT_ERROR;

    if (s_scan_state == WEB_CFG_SCAN_RUNNING)
    {
        /* 软超时自愈：只有在确实卡住时才重新发起，正常扫描中仍然 EBUSY */
        rt_tick_t elapsed = rt_tick_get() - s_scan_start_tick;

        if (elapsed < rt_tick_from_millisecond(WCFG_SCAN_STUCK_MS))
            return -RT_EBUSY;

        LOG_W("scan appeared stuck for %u ms, resetting",
              (unsigned)((elapsed * 1000u) / RT_TICK_PER_SECOND));
        s_scan_state = WEB_CFG_SCAN_IDLE;
    }

    if (s_state == WCFG_ST_CONNECTED) return -RT_EBUSY;

    /* 先置 RUNNING 再释放信号量：调用方随后立即查快照时状态已正确，
     * 不会出现"已请求但看起来仍在 IDLE"的窗口。
     * 投递失败时必须将状态退回 IDLE，否则会永久停留在 RUNNING，
     * 之后每次调用都在上面被拦截为 EBUSY，扫描功能完全失效。 */
    s_scan_start_tick = rt_tick_get();
    s_scan_state = WEB_CFG_SCAN_RUNNING;
    if (rt_sem_release(&s_scan_req) != RT_EOK)
    {
        s_scan_state = WEB_CFG_SCAN_IDLE;
        return -RT_ERROR;
    }
    return 0;
}

int web_cfg_scan_snapshot(web_cfg_ap_t *ap, int max, int *count)
{
    int n = s_scan_cnt;

    if (count != RT_NULL) *count = 0;
    if (s_scan_state != WEB_CFG_SCAN_DONE) return s_scan_state;

    if (ap != RT_NULL && max > 0)
    {
        if (n > max) n = max;
        rt_memcpy(ap, s_scan_ap, (rt_size_t)n * sizeof(web_cfg_ap_t));
        if (count != RT_NULL) *count = n;
    }

    return WEB_CFG_SCAN_DONE;
}

/* ===================== CGI: scan ===================== */

/* CGI 侧：发起扫描并轮询快照，将结果序列化为 JSON 数组。
 * 此处会阻塞 webnet 线程数秒，但 webnet 线程独立于 LVGL，不影响刷屏；
 * 且浏览器本就预期该请求耗时较长。 */
static void cgi_scan(struct webnet_session *session)
{
    web_cfg_ap_t *aps;
    int n = 0, wait_ms = 0, i;
    rt_err_t rc;

    if (s_state == WCFG_ST_CONNECTED)
    {
        send_json(session, "{\"error\":\"sta connected, disconnect first\"}");
        return;
    }

    rc = web_cfg_scan_begin();
    if (rc == -RT_EBUSY)
    {
        /* 已有扫描在跑，直接读它的结果即可 */
    }
    else if (rc != 0)
    {
        send_json(session, "{\"error\":\"scan unavailable\"}");
        return;
    }

    /* 等扫描完成（最多 22 秒，比扫描线程的 20 秒上限略长） */
    while (wait_ms < 22000 && web_cfg_scan_snapshot(RT_NULL, 0, RT_NULL) == WEB_CFG_SCAN_RUNNING)
    {
        rt_thread_mdelay(200);
        wait_ms += 200;
    }

    if (web_cfg_scan_snapshot(RT_NULL, 0, RT_NULL) != WEB_CFG_SCAN_DONE)
    {
        send_json(session, "{\"error\":\"scan failed\"}");
        return;
    }

    /* 堆上取快照再序列化：栈上放 20×40=800 字节对 webnet 的 4KB 栈偏大 */
    aps = (web_cfg_ap_t *)rt_malloc(sizeof(web_cfg_ap_t) * WEB_CFG_SCAN_MAX);
    if (aps == RT_NULL)
    {
        send_json(session, "{\"error\":\"low memory\"}");
        return;
    }
    web_cfg_scan_snapshot(aps, WEB_CFG_SCAN_MAX, &n);

    webnet_session_set_header(session, "application/json", 200, "OK", -1);
    webnet_session_write(session, (const rt_uint8_t *)"[", 1);

    for (i = 0; i < n; i++)
    {
        char buf[96];
        int len = 0;
        const char *s;
        int first = 1;

        if (i > 0) webnet_session_write(session, (const rt_uint8_t *)",", 1);

        webnet_session_write(session, (const rt_uint8_t *)"{\"ssid\":\"", 9);
        for (s = aps[i].ssid; *s; s++)
        {
            unsigned char c = (unsigned char)*s;

            if (c == '"' || c == '\\')
            {
                char esc[2] = { '\\', (char)c };
                webnet_session_write(session, (const rt_uint8_t *)esc, 2);
            }
            else if (c < 0x20)
            {
                char q = '?';
                webnet_session_write(session, (const rt_uint8_t *)&q, 1);
            }
            else
            {
                webnet_session_write(session, (const rt_uint8_t *)&c, 1);
            }
        }
        len = rt_snprintf(buf, sizeof(buf),
                          "\",\"rssi\":%d,\"channel\":%d,\"sec\":%d}",
                          aps[i].rssi, aps[i].channel, aps[i].secure ? 1 : 0);
        webnet_session_write(session, (const rt_uint8_t *)buf, (rt_size_t)len);

        RT_UNUSED(first);
    }

    webnet_session_write(session, (const rt_uint8_t *)"]", 1);
    rt_free(aps);
}

/* ===================== 连接（UI 与 CGI 共用） ===================== */

int web_cfg_connect_begin(const char *ssid, const char *pass)
{
    if (ssid == RT_NULL || ssid[0] == '\0') return -RT_EINVAL;

    if (app_cfg_set(APP_CFG_KEY_WIFI_SSID, ssid) != 0)
        return -RT_EINVAL;

    /* 密码空 = 保持原值（网页回显是掩码，用户抄不出来） */
    if (pass != RT_NULL && pass[0] != '\0')
    {
        if (app_cfg_set(APP_CFG_KEY_WIFI_PASS, pass) != 0)
            return -RT_EINVAL;
    }

    s_need_reconnect = RT_TRUE;
    LOG_I("connect requested via %s",
          (pass != RT_NULL && pass[0]) ? "full credentials" : "saved credentials");
    return 0;
}

void web_cfg_disconnect(void)
{
    s_need_reconnect = RT_FALSE;
    if (rt_wlan_is_connected()) rt_wlan_disconnect();
    s_state = WCFG_ST_IDLE;
}

/* ===================== CGI: wifi ===================== */

/* 用表单中的 ssid/pass 发起连接。立即返回"已提交"，实际连接由状态机
 * 线程执行，因为 HTTP 请求不能阻塞等待 10 秒握手，否则浏览器会超时。
 * 实现复用 web_cfg_connect_begin，与屏上配网走同一路径，
 * 避免两处校验规则不一致。 */
static void cgi_wifi(struct webnet_session *session)
{
    /* 不在这个 CGI 中读取 ssid/pass 参数。
     *
     * 原因：webnet 2.0.3 解析多参数时，对 value 就地 urldecode 会缩短
     * 字符串，但后续参数的指针不会重新对齐，导致第二个及以后的参数
     * 被截断（实测：weather_path 只要不是第一个参数就必然出错）。
     * 因此凭据改由前端通过 /cgi-bin/set 逐字段单独提交，
     * 此处只负责"用当前内存中的凭据发起连接"。
     *
     * 必须先将凭据拷贝到局部缓冲：web_cfg_connect_begin() 内部会再调用
     * app_cfg_set() 写同一块字段，若直接传入 app_cfg_get() 的指针
     * 即为同址 memcpy（未定义行为）。 */
    char ssid[APP_CFG_SSID_LEN];
    char pass[APP_CFG_PASS_LEN];
    const char *src;
    char body[160];

    src = app_cfg_get(APP_CFG_KEY_WIFI_SSID);
    if (src == RT_NULL || src[0] == '\0')
    {
        send_json(session, "{\"error\":\"ssid is empty, save it first\"}");
        return;
    }
    rt_strncpy(ssid, src, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';

    src = app_cfg_get(APP_CFG_KEY_WIFI_PASS);
    if (src != RT_NULL)
    {
        rt_strncpy(pass, src, sizeof(pass) - 1);
        pass[sizeof(pass) - 1] = '\0';
    }
    else
    {
        pass[0] = '\0';
    }

    if (web_cfg_connect_begin(ssid, pass) != 0)
    {
        send_json(session, "{\"error\":\"invalid ssid or too long\"}");
        return;
    }

    rt_snprintf(body, sizeof(body),
                "{\"ok\":1,\"persist\":%s,"
                "\"msg\":\"saved, connecting...\"}",
                app_cfg_persist_ok() ? "true" : "false");
    send_json(session, body);

    LOG_I("wifi credentials updated via web");
}

/* ===================== CGI: reboot ===================== */

extern void rt_hw_cpu_reset(void);

static void cgi_reboot(struct webnet_session *session)
{
    send_json(session, "{\"ok\":1,\"msg\":\"rebooting\"}");
    rt_thread_mdelay(300);      /* 让响应先发出去 */
    rt_hw_cpu_reset();
}

/* ===================== 静态文件兜底 ===================== */

/* webnet 找不到文件时进入本函数。仅当请求为 404 且客户端请求的是首页时
 * 才使用内嵌页，否则 /favicon.ico、/style.css 之类的缺失请求也会收到
 * 一整页 HTML，导致浏览器的 CSS/JS 解析报错，视图渲染失败。
 *
 * 判断依据：path 以 '/' 结尾（目录请求），或末尾为 index.html/index.htm。
 * path 已被 webnet 映射为物理全路径（wn_module.c:558），因此此处看到的是
 * "/sdcard/wfizzz/www/index.html" 这类形式。 */
static rt_bool_t is_index_request(const char *path)
{
    rt_size_t n;

    if (path == RT_NULL) return RT_FALSE;

    n = rt_strlen(path);
    if (n == 0) return RT_TRUE;                 /* 空 = 根 */
    if (path[n - 1] == '/') return RT_TRUE;     /* 目录请求 */

    if (n >= 10 && rt_strcmp(path + n - 10, "index.html") == 0) return RT_TRUE;
    if (n >= 9  && rt_strcmp(path + n - 9,  "index.htm") == 0)  return RT_TRUE;

    return RT_FALSE;
}

static void webnet_err_cb(struct webnet_session *session)
{
    struct webnet_request *req;

    if (session == RT_NULL || session->request == RT_NULL) return;

    req = session->request;
    if (req->result_code != 404) return;
    if (!is_index_request(req->path)) return;

    webnet_session_set_header(session, "text/html; charset=utf-8", 200, "OK",
                              (int)(sizeof(s_fallback_html) - 1));
    webnet_session_write(session, (const rt_uint8_t *)s_fallback_html,
                         sizeof(s_fallback_html) - 1);
    req->result_code = 200;     /* 已处理，别再发一次 404 页 */

    LOG_I("served built-in fallback page");
}

/* ===================== 配网状态机 ===================== */

#define WCFG_THREAD_STACK   4096
#define WCFG_THREAD_PRIO    24
#define WCFG_RETRY_MS       8000
#define WCFG_AP_FALLBACK    3       /* 连续失败几次后起 AP */

/* 扫描线程：调用链是 rt_wlan_scan + whd 驱动（WHD 内部有自己的
 * 工作队列，不占本线程栈），实测主要是 rt_sem/wlan_mgnt 的薄封装，
 * 1.5KB 足够。优先级 26：低于 UI(21)/app_data(24)，扫描期间不抢刷屏 CPU。 */
#define WCFG_SCAN_STACK     1536
#define WCFG_SCAN_PRIO      26

static void web_cfg_ap_start(void)
{
    rt_err_t err;

    if (s_ap_active) return;

    LOG_I("starting SoftAP \"%s\"", WEB_CFG_AP_SSID);

    err = rt_wlan_start_ap(WEB_CFG_AP_SSID, WEB_CFG_AP_PASS);
    if (err == RT_EOK)
    {
        s_ap_active = RT_TRUE;
        s_state = WCFG_ST_AP;
        LOG_I("SoftAP up, browse http://%s/", DHCPD_SERVER_IP);
    }
    else
    {
        LOG_E("SoftAP start failed: %d", err);
    }
}

static void web_cfg_ap_stop(void)
{
    if (!s_ap_active) return;

    if (rt_wlan_ap_stop() == RT_EOK)
    {
        s_ap_active = RT_FALSE;
        LOG_I("SoftAP stopped");
    }
}

static void web_cfg_sta_connect(const char *ssid, const char *pass)
{
    rt_err_t err;

    LOG_I("STA connecting to \"%s\"", ssid);
    s_state = WCFG_ST_CONNECTING;

    /* 启动 STA 前必须关闭 AP：CYW55500 的 STA/AP 可在同一 radio 上共存，
     * 但此处刻意不共存，因为配网期与正常期是互斥的两个阶段，
     * 同时开启会让 DHCP 服务器抢占默认路由。 */
    web_cfg_ap_stop();

    err = rt_wlan_connect(ssid, pass);

    if (err == RT_EOK)
    {
        s_state = WCFG_ST_CONNECTED;
        LOG_I("STA connected, rssi=%d", rt_wlan_get_rssi());
        return;
    }

    LOG_W("STA connect failed: %d", err);
    s_state = WCFG_ST_IDLE;
}

/* 前向声明：状态机线程在网络就绪后才起 webnet */
static int web_cfg_webnet_start(void);

static void web_cfg_entry(void *parameter)
{
    int fails = 0;
    rt_bool_t sta_was_up = RT_FALSE;

    RT_UNUSED(parameter);
    /* wlan0 由 whd 的 INIT_ENV_EXPORT 在独立线程里异步注册 */
    while (rt_device_find(RT_WLAN_DEVICE_STA_NAME) == RT_NULL)
        rt_thread_mdelay(200);

    LOG_I("wlan ready");

    while (1)
    {
        /* 网页改了凭据：强制重连 */
        if (s_need_reconnect)
        {
            s_need_reconnect = RT_FALSE;
            fails = 0;
            if (rt_wlan_is_connected()) rt_wlan_disconnect();
            sta_was_up = RT_FALSE;
        }

        /* 已连接：保持，并确认链路未断开 */
        if (rt_wlan_is_connected() && rt_wlan_is_ready())
        {
            /* webnet 每轮循环都尝试一次：其内部依靠 s_webnet_up 保持幂等，
             * 而启动监听要求 netdev 已 up。ready 事件与 netif up 之间仍存在
             * 极小的窗口，一次失败不应导致端口永久不监听。 */
            web_cfg_webnet_start();

            if (!sta_was_up)
            {
                sta_was_up = RT_TRUE;
                fails = 0;
                s_state = WCFG_ST_CONNECTED;
                s_ap_active = RT_FALSE;

                /* 网络就绪才启动鼾声上报（它依赖 MQTT） */
                if (snore_rt_start() != 0)
                    LOG_W("snore_rt start failed");
            }
            rt_thread_mdelay(WCFG_RETRY_MS);
            continue;
        }

        sta_was_up = RT_FALSE;

        /* 无凭据时直接启动 AP，等待用户配网。
         *
         * 必须先确认 app_cfg 已从 SD 读取完毕：SD 挂载较慢（cfgboot 线程
         * 最多等待 10 秒），这段时间内 wifi_ssid 必然为空，但这是
         * "尚未读到"而非"用户未配"。缺少该判断会在已有凭据的机器上
         * 开机误启 SoftAP。 */
        if (g_app_cfg.wifi_ssid[0] == '\0')
        {
            if (!app_cfg_load_done())
            {
                /* 配置还没读完，什么都不做，等下一轮 */
                rt_thread_mdelay(WCFG_RETRY_MS);
                continue;
            }

            web_cfg_ap_start();
            /* AP 模式下用户需通过该网页配网，因此 webnet 必须启动 */
            web_cfg_webnet_start();
            rt_thread_mdelay(WCFG_RETRY_MS);
            continue;
        }

        /* 有凭据：发起连接；连续失败达到阈值后退化为 AP，使用户可以修改 */
        web_cfg_sta_connect(g_app_cfg.wifi_ssid, g_app_cfg.wifi_pass);

        if (s_state != WCFG_ST_CONNECTED)
        {
            fails++;
            if (fails >= WCFG_AP_FALLBACK)
            {
                LOG_W("%d failed attempts, falling back to SoftAP", fails);
                web_cfg_ap_start();
                web_cfg_webnet_start();
            }
        }
        else
        {
            fails = 0;
        }

        rt_thread_mdelay(WCFG_RETRY_MS);
    }
}

/* ===================== 启动 ===================== */

/* 起 webnet 监听。
 *
 * 时机：webnet_thread 一进函数即执行 socket/bind/listen，任何一步失败都
 * 直接 goto __exit 退出线程（webnet.c:98-183），而 webnet_init() 只负责
 * 创建线程，失败不回传（webnet.c:188-212）。因此"init 成功"并不等于
 * "端口已在监听"。此处等到取得 IP 后再启动，并在启动后自检一次。
 *
 * 幂等：s_webnet_up 为真时直接返回。 */
static int web_cfg_webnet_start(void)
{
    if (s_webnet_up) return 0;

    /* 必须存在一个已 up 的 AF_INET netdev，否则 sal_socket() 中的
     * socket_init() 会在 netdev_get_by_family() 返回 NULL 时直接失败
     * （sal_socket.c:472-482），webnet 线程 socket() 得到 -1 后
     * goto __exit 静默退出，端口始终不监听。
     *
     * 这是"WiFi 与 MQTT 均正常但 80 端口不通"的根本原因：开机时
     * INIT_APP_EXPORT 阶段没有任何 netdev 处于 up 状态。
     *
     * 此处刻意使用 netdev_get_by_family() 而非按名字查询：它与
     * socket_init() 内部的判断条件完全一致（均要求 netdev_is_up），
     * 不会因 STA/AP 的 netif 名字差异而误判。
     * 返回 -RT_EBUSY 让调用方下一轮再试，而不是永久放弃。 */
    if (netdev_get_by_family(AF_INET) == RT_NULL)
    {
        LOG_W("no AF_INET netdev up yet, defer webnet start");
        return -RT_EBUSY;
    }

    /* ---------- webnet ---------- */
    /* root 必须在 webnet_init 之前设，因为 init 内部断言 init_ok == FALSE */
    webnet_set_root(WEB_CFG_ROOT);
    webnet_set_port(80);

    webnet_sessions_set_err_callback(webnet_err_cb);

    webnet_cgi_set_root("/cgi-bin/");
    webnet_cgi_register("status", cgi_status);
    webnet_cgi_register("get",    cgi_get);
    webnet_cgi_register("set",    cgi_set);
    webnet_cgi_register("scan",   cgi_scan);
    webnet_cgi_register("wifi",   cgi_wifi);
    webnet_cgi_register("reboot", cgi_reboot);

    if (webnet_init() != 0)
    {
        LOG_E("webnet init failed");
        return -RT_ERROR;
    }

    s_webnet_up = RT_TRUE;
    LOG_I("http server on :80, root=%s", WEB_CFG_ROOT);

    return 0;
}

/* 起状态机线程。与 webnet 分离：状态机先跑（它负责连 WiFi），
 * webnet 等到拿到 IP 之后才起。 */
int web_cfg_start(void)
{
    static rt_bool_t s_thread_started = RT_FALSE;
    rt_thread_t tid;

    /* ---------- 内核对象与扫描线程 ---------- */
    if (!s_lock_inited)
    {
        s_lock_inited = RT_TRUE;
        if (rt_mutex_init(&s_wifi_lock, "webcfg", RT_IPC_FLAG_PRIO) == RT_EOK)
            s_wifi_lock_ok = RT_TRUE;
    }

    if (!s_scan_inited)
    {
        s_scan_inited = RT_TRUE;

        if (rt_sem_init(&s_scan_req, "wscansem", 0, RT_IPC_FLAG_PRIO) == RT_EOK)
        {
            rt_thread_t scan_tid;

            s_scan_req_ok = RT_TRUE;
            scan_tid = rt_thread_create("wscanth", web_cfg_scan_thread, RT_NULL,
                                        WCFG_SCAN_STACK, WCFG_SCAN_PRIO, 10);
            if (scan_tid != RT_NULL)
                rt_thread_startup(scan_tid);
            else
                LOG_E("scan thread create failed");
        }
        else
        {
            LOG_E("scan semaphore init failed");
        }
    }

    /* ---------- 状态机 ---------- */
    if (s_thread_started) return 0;

    tid = rt_thread_create("webcfg", web_cfg_entry, RT_NULL,
                           WCFG_THREAD_STACK, WCFG_THREAD_PRIO, 10);
    if (tid == RT_NULL)
    {
        LOG_E("thread create failed");
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);
    s_thread_started = RT_TRUE;

    return 0;
}

/* 开机启动。放在 APP 级：此时 app_cfg 已加载（同级按链接顺序，
 * 而 app_cfg.c 排在 web_cfg.c 之前），SD 挂载线程也已在跑。 */
static int web_cfg_auto_start(void)
{
    web_cfg_start();
    return RT_EOK;
}
INIT_APP_EXPORT(web_cfg_auto_start);

/* ===================== msh ===================== */

static void webcfg_info(void)
{
    static const char *const names[] =
    {
        "idle (no credentials)",
        "station connecting",
        "station connected",
        "softap running",
    };
    int st = s_state;

    if (st < 0 || st > 3) st = 0;

    rt_kprintf("webnet  : %s\n", s_webnet_up ? "running" : "stopped");
    rt_kprintf("state   : %s\n", names[st]);
    rt_kprintf("root    : %s\n", WEB_CFG_ROOT);
    rt_kprintf("ssid    : %s\n",
               g_app_cfg.wifi_ssid[0] ? g_app_cfg.wifi_ssid : "(not set)");
    if (s_state == WCFG_ST_AP)
        rt_kprintf("browse  : http://%s/\n", DHCPD_SERVER_IP);
    else
    {
        struct netdev *nd = netdev_get_by_name("w0");
        if (nd == RT_NULL) nd = netdev_default;
        if (nd != RT_NULL && !ip_addr_isany(&nd->ip_addr))
            rt_kprintf("browse  : http://%s/\n", inet_ntoa(nd->ip_addr));
    }
}
MSH_CMD_EXPORT_ALIAS(webcfg_info, webcfg, show web config service state);

/* 手动起 AP，用来在没有手机 App 的情况下调试配网 */
static void webcfg_ap(void)
{
    web_cfg_ap_start();
    rt_kprintf("AP: %s / %s\n", WEB_CFG_AP_SSID, WEB_CFG_AP_PASS);
}
MSH_CMD_EXPORT(webcfg_ap, start provisioning softap);
