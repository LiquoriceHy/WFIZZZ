/*
 * lanctl.c - LAN 设备控制（MQTT）应用逻辑层
 *
 * 实例拓扑：与 snore_rt 各自使用独立的 paho 客户端，连接同一个树莓派
 * mosquitto。本客户端仅订阅一条通配 filter：wfizzz/dev/#（paho 每个客户
 * 端只有一个订阅槽，PKG_PAHOMQTT_SUBSCRIBE_HANDLERS=1），回调中按
 * topic 后缀分流 status 与 reply，并按 <id> 匹配设备表。
 *
 * 线程模型：本模块没有独立线程。paho_start 创建的工作线程负责连接、
 * 重连与收发；命令下发在调用者线程（UI 按钮回调）中经 pipe 模式直接
 * 发送，pipe 写是线程安全的（snore 的 publish 同样跨线程调用）。
 * 设备运行态由互斥锁保护，UI 只获取快照。
 *
 * 订阅时机：paho_mqtt_subscribe 会立即序列化并发送 SUBSCRIBE，未连接时
 * 调用仅失败且不登记 handler，因此该操作放在 online_callback 中执行；
 * 重连时工作线程会自动重订已登记的 filter，此处的重复调用会被去重。
 *
 * 启停语义：start 幂等；stop 为尽力而为，仅在已连接时可停止。DISCONNECT
 * 命令经 pipe 唤醒工作线程（MQTT_CMD 入口检查 isconnected），未连接时
 * 线程处于 _mqtt_restart 重连循环中收不到该命令，此时强制清理缓冲会留下
 * 一个使用悬空指针的线程，因此拒绝停止，待连接建立后再停止。
 */
#include <rtthread.h>
#include <string.h>

#include "lanctl.h"
#include "lanctl_cfg.h"      /* 带数据（设备表），只允许本文件包含 */

#define DBG_TAG "lanctl"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#ifdef PKG_USING_PAHOMQTT
#include "paho_mqtt.h"
#endif
/* cJSON 与 paho 是两个独立开关：reply 内容核对不依赖 MQTT 库 */
#ifdef PKG_USING_CJSON
#include <cJSON.h>
#endif

/* 心跳超时：设备 30-60s 一发 status，留 3 倍余量 */
#define LANCTL_STALE_MS   150000u

typedef struct
{
    const lanctl_dev_cfg_t *cfg;   /* 静态配置（设备表项） */
    rt_bool_t  seen;               /* 收到过该设备的任何报文 */
    rt_bool_t  online_flag;        /* 最近一条 status 的 online 字段 */
    rt_tick_t  last_seen;          /* 最后一次报文的 tick */
    char       ip[16];
    char       last_cmd[48];       /* 最近下发的 JSON（本地记录） */
    char       last_reply[96];     /* 最近一条 reply 原文 */
    rt_uint32_t reply_seq;         /* 收到 reply 的累计计数（AI 等回包用） */
} lanctl_dev_t;

static struct rt_mutex s_lock;
static rt_bool_t       s_inited;
static lanctl_dev_t    s_devs[LANCTL_DEV_MAX];
static int             s_dev_cnt;

#ifdef PKG_USING_PAHOMQTT
static MQTTClient s_client;
static char       s_client_id[24];
static volatile rt_bool_t s_subscribed;   /* online 回调里订阅成功过 */
#endif

/* tick 差转毫秒（无符号减法，天然处理回绕） */
static rt_uint32_t tick_delta_ms(rt_tick_t now, rt_tick_t last)
{
    return (rt_uint32_t)(now - last) * 1000u / RT_TICK_PER_SECOND;
}

/* 首次调用时建锁并从设备表填 s_devs（多线程都可能先进来，双检） */
static void lanctl_ensure(void)
{
    int i;

    if (s_inited)
        return;

    rt_enter_critical();
    if (!s_inited)
    {
        rt_mutex_init(&s_lock, "lanctl", RT_IPC_FLAG_FIFO);
        for (i = 0; i < LANCTL_DEV_MAX && s_lanctl_devs[i].id != RT_NULL; i++)
            s_devs[i].cfg = &s_lanctl_devs[i];
        s_dev_cnt = i;
        s_inited = RT_TRUE;
    }
    rt_exit_critical();
}

#ifdef PKG_USING_PAHOMQTT

static void lanctl_handle_status(lanctl_dev_t *d, const char *payload)
{
    cJSON *root, *item;

    d->seen      = RT_TRUE;
    d->last_seen = rt_tick_get();

    root = cJSON_Parse(payload);
    if (root == RT_NULL)
        return;                       /* 非 JSON 的 status：仅当一次心跳 */

    item = cJSON_GetObjectItem(root, "online");
    if (item != RT_NULL && cJSON_IsBool(item))
        d->online_flag = cJSON_IsTrue(item) ? RT_TRUE : RT_FALSE;

    item = cJSON_GetObjectItem(root, "ip");
    if (item != RT_NULL && cJSON_IsString(item) && item->valuestring != RT_NULL)
    {
        rt_strncpy(d->ip, item->valuestring, sizeof(d->ip) - 1);
        d->ip[sizeof(d->ip) - 1] = '\0';
    }

    cJSON_Delete(root);
}

/* 通配订阅回调（paho 工作线程上下文）：topic = wfizzz/dev/<id>/<kind> */
static void lanctl_msg_cb(MQTTClient *c, MessageData *data)
{
    char topic[48];
    char payload[192];
    const char *id, *kind;
    int i, len, id_len;

    RT_UNUSED(c);

    /* topic/payload 都指向 paho 读缓冲，只在回调内有效，先拷出来 */
    len = data->topicName->lenstring.len;
    if (len <= 0 || len >= (int)sizeof(topic))
        return;
    rt_memcpy(topic, data->topicName->lenstring.data, len);
    topic[len] = '\0';

    len = (int)data->message->payloadlen;
    if (len < 0)
        return;
    if (len > (int)sizeof(payload) - 1)
        len = (int)sizeof(payload) - 1;
    rt_memcpy(payload, data->message->payload, len);
    payload[len] = '\0';

    len = (int)sizeof(LANCTL_TOPIC_PREFIX) - 1;
    if (rt_strncmp(topic, LANCTL_TOPIC_PREFIX, len) != 0)
        return;
    id   = topic + len;
    kind = strchr(id, '/');
    if (kind == RT_NULL)
        return;
    id_len = (int)(kind - id);
    kind++;

    for (i = 0; i < s_dev_cnt; i++)
    {
        lanctl_dev_t *d = &s_devs[i];

        if ((int)rt_strlen(d->cfg->id) != id_len ||
            rt_memcmp(d->cfg->id, id, id_len) != 0)
            continue;

        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        if (rt_strcmp(kind, "status") == 0)
        {
            lanctl_handle_status(d, payload);
        }
        else if (rt_strcmp(kind, "reply") == 0)
        {
            /* 收到回包说明设备活着，一并刷新心跳。
             * 定长 memcpy 而非 rt_strncpy：源最长 191 字节，strncpy
             * 截断拷贝会触发 -Wstringop-truncation 告警 */
            int rlen = rt_strlen(payload);

            if (rlen > (int)sizeof(d->last_reply) - 1)
                rlen = (int)sizeof(d->last_reply) - 1;
            rt_memcpy(d->last_reply, payload, rlen);
            d->last_reply[rlen] = '\0';
            d->seen      = RT_TRUE;
            d->last_seen = rt_tick_get();
            d->reply_seq++;           /* 唤醒 lanctl_wait_reply 的轮询 */
        }
        rt_mutex_release(&s_lock);

        LOG_I("dev %s <- %s: %s", d->cfg->id, kind, payload);
        return;
    }

    LOG_W("ignore unknown device topic: %s", topic);
}

static void lanctl_online_cb(MQTTClient *c)
{
    int rc;

    /* 订阅必须在连上后做（见文件头注释）；重连时 paho 自动重订已登记
     * 的 filter，这里的重复调用会被内部去重。 */
    rc = paho_mqtt_subscribe(&s_client, QOS1, LANCTL_TOPIC_FILTER, lanctl_msg_cb);
    if (rc == 0)
        s_subscribed = RT_TRUE;
    else
        LOG_E("subscribe %s failed (%d)", LANCTL_TOPIC_FILTER, rc);

    RT_UNUSED(c);
}

static void lanctl_offline_cb(MQTTClient *c)
{
    /* 不动 app_data_set_mqtt：状态栏 MQTT 指示归 snore 客户端所有 */
    LOG_W("mqtt offline (auto reconnecting)");
    RT_UNUSED(c);
}

#endif /* PKG_USING_PAHOMQTT */

/* ---------------- 对外 API ---------------- */

int lanctl_start(void)
{
#ifdef PKG_USING_PAHOMQTT
    MQTTPacket_connectData condata = MQTTPacket_connectData_initializer;

    lanctl_ensure();

    if (s_client.uri != RT_NULL)
        return 0;                     /* 已启动；paho 内部自管重连 */

    if (LANCTL_MQTT_URI[0] == '\0')
    {
        LOG_W("mqtt uri not configured, lan control disabled");
        return -RT_EINVAL;
    }

    rt_snprintf(s_client_id, sizeof(s_client_id),
                "m55_lanctl_%08x", (rt_uint32_t)rt_tick_get());

    rt_memset(&s_client, 0, sizeof(s_client));
    s_client.uri = LANCTL_MQTT_URI;
    rt_memcpy(&s_client.condata, &condata, sizeof(condata));
    s_client.condata.clientID.cstring = s_client_id;
    s_client.condata.username.cstring = LANCTL_MQTT_USER;
    s_client.condata.password.cstring = LANCTL_MQTT_PASS;
    s_client.condata.keepAliveInterval = 30;
    s_client.condata.cleansession = 1;

    s_client.buf_size = s_client.readbuf_size = 1024;
    s_client.buf = rt_calloc(1, s_client.buf_size);
    s_client.readbuf = rt_calloc(1, s_client.readbuf_size);
    if (s_client.buf == RT_NULL || s_client.readbuf == RT_NULL)
    {
        LOG_E("buffer alloc failed");
        if (s_client.buf)     { rt_free(s_client.buf);     s_client.buf = RT_NULL; }
        if (s_client.readbuf) { rt_free(s_client.readbuf); s_client.readbuf = RT_NULL; }
        s_client.uri = RT_NULL;
        return -RT_ENOMEM;
    }

    s_client.online_callback  = lanctl_online_cb;
    s_client.offline_callback = lanctl_offline_cb;

    paho_mqtt_start(&s_client);
    LOG_I("started id=%s broker=%s", s_client_id, LANCTL_MQTT_URI);
    return 0;
#else
    return -RT_ENOSYS;
#endif
}

void lanctl_stop(void)
{
#ifdef PKG_USING_PAHOMQTT
    int i;

    if (s_client.uri == RT_NULL)
        return;

    if (!s_client.isconnected)
    {
        LOG_W("cannot stop while not connected (broker unreachable?), retry later");
        return;
    }

    paho_mqtt_stop(&s_client);

    /* 工作线程收到 DISCONNECT 后走退出路径：释放 buf/handler 后线程
     * 结束。messageHandlers[0].topicFilter 被 free 置 NULL 只发生在这条
     * 退出路径（重连路径不释放），可当"线程已收尾"的信号。 */
    if (s_subscribed)
    {
        for (i = 0; i < 100; i++)     /* 最多等 1s */
        {
            if (s_client.messageHandlers[0].topicFilter == RT_NULL)
                break;
            rt_thread_mdelay(10);
        }
        if (s_client.messageHandlers[0].topicFilter != RT_NULL)
        {
            LOG_W("worker thread did not exit in 1s, left running");
            return;
        }
    }
    else
    {
        /* 连上过但订阅从未成功（罕见）：没有可靠的退出信号，等一拍 */
        rt_thread_mdelay(300);
    }

    /* 缓冲已由线程释放，这里只复位本模块持有的指针/标记，防止下次
     * start 把悬空指针当成"已启动"。 */
    s_client.uri         = RT_NULL;
    s_client.buf         = RT_NULL;
    s_client.readbuf     = RT_NULL;
    s_client.isconnected = 0;
    s_subscribed         = RT_FALSE;
    LOG_I("stopped");
#endif
}

rt_bool_t lanctl_is_running(void)
{
#ifdef PKG_USING_PAHOMQTT
    return (s_client.uri != RT_NULL) ? RT_TRUE : RT_FALSE;
#else
    return RT_FALSE;
#endif
}

rt_bool_t lanctl_mqtt_online(void)
{
#ifdef PKG_USING_PAHOMQTT
    return (s_client.uri != RT_NULL && s_client.isconnected) ? RT_TRUE : RT_FALSE;
#else
    return RT_FALSE;
#endif
}

void lanctl_get_status(char *buf, int len)
{
    int i, up = 0;

    lanctl_ensure();

    if (buf == RT_NULL || len <= 0)
        return;

#ifdef PKG_USING_PAHOMQTT
    if (s_client.uri == RT_NULL)
    {
        rt_snprintf(buf, len, "stopped");
        return;
    }
    if (!s_client.isconnected)
    {
        rt_snprintf(buf, len, "connecting");
        return;
    }

    if (rt_mutex_take(&s_lock, rt_tick_from_millisecond(50)) == RT_EOK)
    {
        for (i = 0; i < s_dev_cnt; i++)
            if (s_devs[i].seen && s_devs[i].online_flag &&
                tick_delta_ms(rt_tick_get(), s_devs[i].last_seen) < LANCTL_STALE_MS)
                up++;
        rt_mutex_release(&s_lock);
    }
    rt_snprintf(buf, len, "online, %d/%d dev up", up, s_dev_cnt);
#else
    RT_UNUSED(i);
    rt_snprintf(buf, len, "no MQTT");
#endif
}

int lanctl_dev_count(void)
{
    lanctl_ensure();
    return s_dev_cnt;
}

void lanctl_dev_snapshot(int idx, lanctl_dev_snapshot_t *out)
{
    lanctl_dev_t *d;
    rt_tick_t now = rt_tick_get();

    lanctl_ensure();

    if (out == RT_NULL)
        return;
    rt_memset(out, 0, sizeof(*out));

    if (idx < 0 || idx >= s_dev_cnt)
        return;

    d = &s_devs[idx];

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    out->seen  = d->seen;
    out->age_s = d->seen ? tick_delta_ms(now, d->last_seen) / 1000u : 0;
    out->online = (d->seen && d->online_flag &&
                   tick_delta_ms(now, d->last_seen) < LANCTL_STALE_MS)
                      ? RT_TRUE : RT_FALSE;
    rt_strncpy(out->ip, d->ip, sizeof(out->ip) - 1);
    out->ip[sizeof(out->ip) - 1] = '\0';
    rt_strncpy(out->last_cmd, d->last_cmd, sizeof(out->last_cmd) - 1);
    out->last_cmd[sizeof(out->last_cmd) - 1] = '\0';
    rt_strncpy(out->last_reply, d->last_reply, sizeof(out->last_reply) - 1);
    out->last_reply[sizeof(out->last_reply) - 1] = '\0';
    rt_mutex_release(&s_lock);

    /* id/name 来自只读静态表，锁外拷 */
    rt_strncpy(out->id, d->cfg->id, sizeof(out->id) - 1);
    out->id[sizeof(out->id) - 1] = '\0';
    rt_strncpy(out->name, d->cfg->name, sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
}

int lanctl_dev_cmd_count(int idx)
{
    int n = 0;

    lanctl_ensure();

    if (idx < 0 || idx >= s_dev_cnt)
        return 0;
    while (n < LANCTL_CMD_MAX && s_devs[idx].cfg->cmds[n].label != RT_NULL)
        n++;
    return n;
}

const char *lanctl_dev_cmd_label(int idx, int cmd)
{
    lanctl_ensure();

    if (idx < 0 || idx >= s_dev_cnt || cmd < 0 || cmd >= LANCTL_CMD_MAX)
        return RT_NULL;
    return s_devs[idx].cfg->cmds[cmd].label;   /* 未用到的槽为 RT_NULL */
}

int lanctl_dev_send(int idx, int cmd)
{
#ifdef PKG_USING_PAHOMQTT
    lanctl_dev_t *d;
    char topic[48];
    const char *json;

    lanctl_ensure();

    if (idx < 0 || idx >= s_dev_cnt || cmd < 0 || cmd >= LANCTL_CMD_MAX)
        return -RT_EINVAL;
    if (s_devs[idx].cfg->cmds[cmd].label == RT_NULL ||
        s_devs[idx].cfg->cmds[cmd].json == RT_NULL)
        return -RT_EINVAL;

    if (s_client.uri == RT_NULL || !s_client.isconnected)
    {
        LOG_W("send '%s': mqtt offline, dropped",
              s_devs[idx].cfg->cmds[cmd].label);
        return -RT_ERROR;
    }

    d = &s_devs[idx];
    json = d->cfg->cmds[cmd].json;
    rt_snprintf(topic, sizeof(topic), LANCTL_TOPIC_PREFIX "%s/cmd", d->cfg->id);

    if (paho_mqtt_publish(&s_client, QOS1, topic, json) != 0)
    {
        LOG_E("publish to %s failed", topic);
        return -RT_ERROR;
    }

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    rt_strncpy(d->last_cmd, json, sizeof(d->last_cmd) - 1);
    d->last_cmd[sizeof(d->last_cmd) - 1] = '\0';
    rt_mutex_release(&s_lock);

    LOG_I("dev %s <- cmd: %s", d->cfg->id, json);
    return 0;
#else
    RT_UNUSED(idx);
    RT_UNUSED(cmd);
    return -RT_ENOSYS;
#endif
}

/* ---------------- AI / MCP 接口 ---------------- */

/* 按 name 找命令槽；找不到返回 -1。未用槽位的 name 为 RT_NULL。 */
static int lanctl_cmd_by_name(int idx, const char *name)
{
    int i, n;

    n = lanctl_dev_cmd_count(idx);
    for (i = 0; i < n; i++)
    {
        const char *cn = s_devs[idx].cfg->cmds[i].name;

        if (cn != RT_NULL && rt_strcmp(cn, name) == 0)
            return i;
    }
    return -1;
}

/* 忽略大小写与 '-_ ' 分隔符的字符串相等：AI 常把 "ESP32-C6" 写成
 * "esp32-c6" 或 "esp32c6"，按 id 匹配不能因为拼写细节失败。 */
static rt_bool_t str_eq_loose(const char *a, const char *b)
{
    for (;;)
    {
        char ca, cb;

        while (*a == '-' || *a == '_' || *a == ' ') a++;
        while (*b == '-' || *b == '_' || *b == ' ') b++;

        ca = *a;
        cb = *b;

        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');

        if (ca != cb)
            return RT_FALSE;
        if (ca == '\0')
            return RT_TRUE;

        a++;
        b++;
    }
}

int lanctl_find_dev(const char *dev)
{
    int i;

    lanctl_ensure();

    if (dev == RT_NULL || dev[0] == '\0')
        return s_dev_cnt == 1 ? 0 : -1;   /* 省略设备名时只在无歧义下生效 */

    for (i = 0; i < s_dev_cnt; i++)
    {
        if (str_eq_loose(s_devs[i].cfg->id, dev) ||
            str_eq_loose(s_devs[i].cfg->name, dev))
            return i;
    }
    return -1;
}

void lanctl_dev_cmd_names(int idx, char *buf, int len)
{
    int i, n, used = 0;

    lanctl_ensure();

    if (buf == RT_NULL || len <= 0)
        return;
    buf[0] = '\0';

    if (idx < 0 || idx >= s_dev_cnt)
        return;

    n = lanctl_dev_cmd_count(idx);
    for (i = 0; i < n; i++)
    {
        const char *cn = s_devs[idx].cfg->cmds[i].name;
        int need;

        if (cn == RT_NULL)
            continue;

        /* 先算所需空间再写：截断会切出半个命令名，宁可整条不写。
         * 分隔逗号 + 命令名 + '\0' */
        need = (used ? 1 : 0) + (int)rt_strlen(cn) + 1;
        if (used + need > len)
            break;

        if (used)
            buf[used++] = ',';
        while (*cn != '\0')
            buf[used++] = *cn++;
        buf[used] = '\0';
    }
}

rt_uint32_t lanctl_reply_seq(int idx)
{
    rt_uint32_t seq = 0;

    lanctl_ensure();

    if (idx < 0 || idx >= s_dev_cnt)
        return 0;

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    seq = s_devs[idx].reply_seq;
    rt_mutex_release(&s_lock);
    return seq;
}

int lanctl_send_name(int idx, const char *name, rt_uint32_t *seq_out,
                     const char **proto_out)
{
    int cmd;
    rt_uint32_t seq;

    lanctl_ensure();

    if (name == RT_NULL || idx < 0 || idx >= s_dev_cnt)
        return -RT_EINVAL;

    cmd = lanctl_cmd_by_name(idx, name);
    if (cmd < 0)
        return -RT_ENOENT;

    /* 先取序号再发布：发布到设备回包之间有网络往返，这个窗口里
     * 设备不可能先回包，因此该序号一定是"本次命令之前"的值 */
    seq = lanctl_reply_seq(idx);

    if (lanctl_dev_send(idx, cmd) != 0)
        return -RT_ERROR;

    if (seq_out != RT_NULL)
        *seq_out = seq;
    if (proto_out != RT_NULL)
        *proto_out = s_devs[idx].cfg->cmds[cmd].proto;

    return 0;
}

/* 判断一条 reply 是否属于 expect_cmd 这条命令（reply 的 "cmd" 字段回显
 * 协议命令名）。非 JSON 或缺 cmd 字段时返回真：宁可返回一条不确定的
 * 回包，也不要让调用方白等到超时。 */
static rt_bool_t reply_matches(const char *reply, const char *expect_cmd)
{
#ifdef PKG_USING_CJSON
    cJSON *root, *item;
    rt_bool_t match;

    if (expect_cmd == RT_NULL)
        return RT_TRUE;

    root = cJSON_Parse(reply);
    if (root == RT_NULL)
        return RT_TRUE;

    item = cJSON_GetObjectItem(root, "cmd");
    if (item == RT_NULL || !cJSON_IsString(item) || item->valuestring == RT_NULL)
        match = RT_TRUE;
    else
        match = (rt_strcmp(item->valuestring, expect_cmd) == 0) ? RT_TRUE
                                                               : RT_FALSE;

    cJSON_Delete(root);
    return match;
#else
    /* 没有 cJSON 时无法解析归属，退化为"任何新回包都算" */
    RT_UNUSED(reply);
    RT_UNUSED(expect_cmd);
    return RT_TRUE;
#endif
}

int lanctl_wait_reply(int idx, const char *expect_cmd, rt_uint32_t after_seq,
                      int timeout_ms, char *out, int len)
{
    rt_tick_t deadline;

    lanctl_ensure();

    if (out == RT_NULL || len <= 0 || idx < 0 || idx >= s_dev_cnt)
        return -RT_EINVAL;

    out[0] = '\0';

    if (timeout_ms < 0)
        timeout_ms = 0;

    deadline = rt_tick_get() + rt_tick_from_millisecond(timeout_ms);

    /* 轮询而非信号量：paho 回调线程不应 take 本模块的等待原语（会引入
     * 反向依赖），10ms 粒度对本场景（设备秒级响应）足够。 */
    for (;;)
    {
        rt_bool_t got = RT_FALSE;

        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        /* 序号判新旧 + cmd 判归属。只靠序号会把"上一条命令迟到的回包"
         * 当成本次结果，让 AI 误报已执行。 */
        if (s_devs[idx].reply_seq != after_seq &&
            reply_matches(s_devs[idx].last_reply, expect_cmd))
        {
            int rlen = rt_strlen(s_devs[idx].last_reply);

            if (rlen > len - 1)
                rlen = len - 1;
            rt_memcpy(out, s_devs[idx].last_reply, rlen);
            out[rlen] = '\0';
            got = RT_TRUE;
        }
        rt_mutex_release(&s_lock);

        if (got)
            return 1;

        if ((rt_int32_t)(deadline - rt_tick_get()) <= 0)
            return 0;

        rt_thread_mdelay(10);
    }
}

/* 开机自启：paho 内部按 5s 重试，WiFi 起来后自然连上，抽屉卡片即可
 * 实时显示设备在线状态。 */
static int lanctl_init(void)
{
    lanctl_ensure();
    lanctl_start();
    return 0;
}
INIT_APP_EXPORT(lanctl_init);
