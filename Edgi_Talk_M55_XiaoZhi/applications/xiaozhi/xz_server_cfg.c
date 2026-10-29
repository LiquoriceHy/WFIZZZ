/*
 * xz_server_cfg.c - 小智服务端连接参数的运行时读取
 *
 * 详见 xz_server_cfg.h 的设计说明。此处强调一处实现细节：
 *
 * 不做缓存。每次调用都重新从 app_cfg 取值，因为用户可能在网页上修改
 * 地址后不重启即重连。app_cfg_get() 本身是无锁的直接指针读取（单次
 * memcpy 量级的写，见 app_cfg.c 的线程安全说明），开销可忽略；且这些
 * 函数每次连接尝试仅调用数次，而非每帧调用。
 */
#include <rtthread.h>
#include <stdlib.h>
#include <string.h>

#include "xz_server_cfg.h"
#include "app_cfg.h"

/* 官方云兜底值，与 xiaozhi.h 中的宏保持一致。那几个宏仍被其它位置引用
 * （MCP 的 session 拼装等），不能直接删除。 */
#define XZ_DEF_HOST   "api.tenclass.net"
#define XZ_DEF_PATH   "/xiaozhi/v1/"
#define XZ_DEF_TOKEN  "Bearer 12345678"
#define XZ_DEF_PORT   443

const char *xz_server_host(void)
{
    const char *v = app_cfg_get(APP_CFG_KEY_XZ_HOST);

    return (v != RT_NULL && v[0] != '\0') ? v : XZ_DEF_HOST;
}

const char *xz_server_path(void)
{
    const char *v = app_cfg_get(APP_CFG_KEY_XZ_PATH);

    return (v != RT_NULL && v[0] != '\0') ? v : XZ_DEF_PATH;
}

const char *xz_server_token(void)
{
    const char *v = app_cfg_get(APP_CFG_KEY_XZ_TOKEN);

    /* 空 token 在自建服务端（auth.enabled=false）是合法的。
     *
     * 必须返回 RT_NULL 而不是空串：wsock_connect 的判断是
     * `if (bearer_token) sprintf("Authorization: bearer %s")`，
     * 传空串会发出 "Authorization: bearer " 这个畸形头（有 key 无值），
     * 部分 ws 实现会直接 400。传 NULL 则整个头不发。 */
    if (v == RT_NULL)
        return XZ_DEF_TOKEN;

    return (v[0] != '\0') ? v : RT_NULL;
}

/* 解析一个端口字段，失败回退 def */
static rt_uint16_t xz_parse_port(const char *key, rt_uint16_t def)
{
    const char *v = app_cfg_get(key);
    long p;

    if (v == RT_NULL || v[0] == '\0')
        return def;

    p = strtol(v, RT_NULL, 10);

    /* 越界一律回退：0 或 70000 这种值是用户手滑，直接用会让连接
     * 连到一个莫名其妙的端口，报错信息完全看不出根因。 */
    if (p <= 0 || p > 65535)
    {
        rt_kprintf("[xz.cfg] bad %s \"%s\", fallback to %u\n",
                   key, v, (unsigned)def);
        return def;
    }

    return (rt_uint16_t)p;
}

rt_uint16_t xz_server_port(void)
{
    return xz_parse_port(APP_CFG_KEY_XZ_PORT, XZ_DEF_PORT);
}

const char *xz_server_ota_port(void)
{
    /* OTA 端口留空时：
     *   - TLS 模式（官方云）→ 和 ws 同端口 443，行为与改动前一致
     *   - 明文模式（自建）  → 默认 8003，这是服务端 config.yaml 的 http_port */
    const char *v = app_cfg_get(APP_CFG_KEY_XZ_OTA_PORT);
    static char s_buf[APP_CFG_PORT_LEN];

    if (v != RT_NULL && v[0] != '\0')
        return v;

    rt_snprintf(s_buf, sizeof(s_buf), "%u",
                xz_server_ssl() ? (unsigned)xz_server_port() : 8003u);
    return s_buf;
}

rt_bool_t xz_server_ssl(void)
{
    const char *v = app_cfg_get(APP_CFG_KEY_XZ_SSL);

    if (v == RT_NULL || v[0] == '\0')
        return RT_TRUE;   /* 默认官方云是 wss */

    /* 仅将 "0" 视为关闭。填写 "false" 同样按开启处理：宁可多一次 TLS
     * 握手失败促使使用者查看日志，也不静默将加密连接降级为明文。 */
    return (v[0] == '0') ? RT_FALSE : RT_TRUE;
}

char *xz_server_ota_url(char *out, rt_size_t len)
{
    rt_uint16_t port;

    if (out == RT_NULL || len == 0)
        return out;

    port = (rt_uint16_t)strtol(xz_server_ota_port(), RT_NULL, 10);
    if (port == 0)
        port = 8003;

    rt_snprintf(out, len, "%s://%s:%u/xiaozhi/ota/",
                xz_server_ssl() ? "https" : "http",
                xz_server_host(),
                (unsigned)port);

    return out;
}

void xz_server_dump(void)
{
    const char *token = xz_server_token();

    rt_kprintf("----------------------------------------\n");
    rt_kprintf("xiaozhi server\n");
    rt_kprintf("  host    : %s\n", xz_server_host());
    rt_kprintf("  ws port : %u\n", (unsigned)xz_server_port());
    rt_kprintf("  ws path : %s\n", xz_server_path());
    rt_kprintf("  ota port: %s\n", xz_server_ota_port());
    rt_kprintf("  ssl     : %s\n", xz_server_ssl() ? "yes (wss/https)"
                                                    : "no (ws/http)");
    /* token 仅报告是否存在，不打印明文，因为 rt_kprintf 为串口明文输出 */
    rt_kprintf("  token   : %s\n", (token != RT_NULL && token[0] != '\0')
                                   ? "(set)" : "(empty)");
    rt_kprintf("----------------------------------------\n");
}
MSH_CMD_EXPORT_ALIAS(xz_server_dump, xzcfg, dump xiaozhi server config);
