/*
 * app_cfg.c - 运行时配置中枢的实现
 *
 * 设计取舍：
 *   1) 存 JSON 而非二进制结构体。字段增删不影响旧文件，掉电也不会因
 *      结构体版本不符而读出无效数据。代价是需要一个简易解析器：只处理
 *      扁平 {"key":"value"}，不建树、不递归，40 行即可，省下 cJSON 的堆开销。
 *   2) 写入走 tmp + rename。elmfat 的 f_rename 在目标存在时先删除再改名
 *      （ff.c:5693 的分支只在 FA_CREATE_NEW 时返回 FR_EXIST），因此
 *      rename 覆盖是原子的，掉电最坏丢失本次修改而不会写坏主文件。
 *   3) 长度校验用 sizeof(field) - 1，因此网页表单的 maxlength 必须与
 *      app_cfg.h 中的 *(_LEN) 一致。超长字符会被拒绝而非截断，
 *      静默截断会让用户误以为密钥已正确保存。
 *
 * 线程安全：
 *   配置改动来自 webnet 线程，读取来自 UI 与工作线程。此处用一把互斥量
 *   保护 g_app_cfg 的写操作与落盘；读取（app_cfg_get）不加锁，因为
 *   字符串写入是单次 memcpy，读者只会看到旧值或新值，不会看到半个串。
 *   该设计有实际必要：app_cfg_save 会执行数十毫秒的 flash 写，期间
 *   LVGL 线程读配置不应被阻塞。
 *
 * 约束：
 *   - /sdcard/wfizzz 不会自动存在，必须 mkdir。elmfat 的 mkdir 不支持
 *     递归，因此只能创建一级目录（本模块正好只需要一级）。
 *   - SD 卡拔出时 open 返回负数，不能视为致命错误，整机仍需继续运行。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>

#include "app_cfg.h"

#define DBG_TAG "app.cfg"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

app_cfg_t g_app_cfg;

/* 落盘能力：初始化时探测一次。RT_FALSE 表示配置只在 RAM 生效 */
static rt_bool_t s_persist_ok = RT_FALSE;
/* 开机读取是否已有结论（读到了 / 卡上没有 / 超时放弃）。
 * 与 s_persist_ok 的区别见 app_cfg_load_done() 的注释：
 * SD 还没挂载时 s_persist_ok 是假，此时配置全是默认值，
 * 调用方不能把"wifi_ssid 为空"当成"用户没配网"。 */
static volatile rt_bool_t s_load_done = RT_FALSE;
static rt_bool_t s_inited = RT_FALSE;
static struct rt_mutex s_lock;
static rt_bool_t s_lock_ok = RT_FALSE;
/* s_lock 是否已 init 过。与 s_inited 分开：后者管"配置是否已加载"
 * （cfg_reload 会复位它以便重新读取），前者管"内核对象是否已创建"
 * （不得再次 init，否则命中 rt_object_init 的断言）。 */
static rt_bool_t s_lock_inited = RT_FALSE;

/* ===================== 字段表 ===================== */
/* 所有字段的元信息集中一处：解析、回显、校验、落盘都从这里取，
 * 避免新增字段时漏改某个 switch。 */
struct cfg_field
{
    const char *key;
    rt_uint16_t offset;
    rt_uint16_t size;
    rt_bool_t   secret;     /* 回显时掩码 */
};

#define CFG_FIELD(k, f, secret) \
    { (k), (rt_uint16_t)offsetof(app_cfg_t, f), (rt_uint16_t)sizeof(((app_cfg_t *)0)->f), (secret) }

static const struct cfg_field s_fields[] =
{
    CFG_FIELD(APP_CFG_KEY_WIFI_SSID,  wifi_ssid,      RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_WIFI_PASS,  wifi_pass,      RT_TRUE),
    CFG_FIELD(APP_CFG_KEY_XZ_HOST,    xiaozhi_host,   RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_XZ_PATH,    xiaozhi_path,   RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_XZ_TOKEN,   xiaozhi_token,  RT_TRUE),
    CFG_FIELD(APP_CFG_KEY_XZ_PORT,    xiaozhi_port,   RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_XZ_OTA_PORT, xiaozhi_ota_port, RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_XZ_SSL,     xiaozhi_ssl,    RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_MEMO_HOST,  memo_host,      RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_MEMO_TOKEN, memo_token,     RT_TRUE),
    CFG_FIELD(APP_CFG_KEY_WEA_HOST,   weather_host,   RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_WEA_PATH,   weather_path,   RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_WEA_KEY,    weather_key,    RT_TRUE),
    CFG_FIELD(APP_CFG_KEY_WEA_CITY,   weather_city,   RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_LLM_HOST,   llm_host,       RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_LLM_PATH,   llm_path,       RT_FALSE),
    CFG_FIELD(APP_CFG_KEY_LLM_KEY,    llm_key,        RT_TRUE),
    CFG_FIELD(APP_CFG_KEY_LLM_MODEL,  llm_model,      RT_FALSE),
};

#define CFG_FIELD_NUM   (sizeof(s_fields) / sizeof(s_fields[0]))

static char *cfg_slot(const struct cfg_field *f)
{
    return ((char *)&g_app_cfg) + f->offset;
}

static const struct cfg_field *cfg_find(const char *key)
{
    rt_size_t i;

    if (key == RT_NULL) return RT_NULL;

    for (i = 0; i < CFG_FIELD_NUM; i++)
    {
        if (rt_strcmp(s_fields[i].key, key) == 0)
            return &s_fields[i];
    }
    return RT_NULL;
}

/* ===================== 默认值 ===================== */
/* 只放地址与端口这类非机密默认值。真实 token 与密码一律留空，
 * 由用户在配网页填写后落至 SD 卡，不写入源码。 */
static void cfg_set_defaults(void)
{
    rt_memset(&g_app_cfg, 0, sizeof(g_app_cfg));

    /* WiFi：留空表示"未配网"，P1 据此起 SoftAP 让用户填 */
    g_app_cfg.wifi_ssid[0] = '\0';
    g_app_cfg.wifi_pass[0] = '\0';

    /* 小智语音服务：默认官方云（wss + 443）。
     *
     * 改为自建服务端时只需修改这五项：host 填树莓派 IP，port 填 8000，
     * path 保持 /xiaozhi/v1/，ssl 填 0（局域网内使用纯 ws，省去 TLS 握手，
     * 因为主堆仅剩约 40KB，mbedTLS 的握手缓冲是不必要的负担）。
     * token 默认为官方占位值，自建服务端未开认证时会忽略它。 */
    rt_strncpy(g_app_cfg.xiaozhi_host, "api.tenclass.net", APP_CFG_HOST_LEN - 1);
    rt_strncpy(g_app_cfg.xiaozhi_path, "/xiaozhi/v1/", APP_CFG_PATH_LEN - 1);
    rt_strncpy(g_app_cfg.xiaozhi_port, "443", APP_CFG_PORT_LEN - 1);
    rt_strncpy(g_app_cfg.xiaozhi_ota_port, "443", APP_CFG_PORT_LEN - 1);
    rt_strncpy(g_app_cfg.xiaozhi_ssl, "1", APP_CFG_FLAG_LEN - 1);

    /* 墨墨：域名固定，token 必须由用户填 */
    rt_strncpy(g_app_cfg.memo_host, "open.maimemo.com", APP_CFG_HOST_LEN - 1);

    /* 天气：心知天气（Seniverse）。2026-10-26 定案，理由见
     * docs/qweather-integration.md：响应不压缩，证书链落在板内已嵌的
     * DigiCert Global Root G2 上，而和风天气三项均不满足。
     *
     * 此处只放公网域名与纯路径，key 与城市由用户填写。
     * 路径中不含 location：weather.c 拼 URL 时将 city 追加为
     * &location=<city>&language=en&unit=c。使用 en 是有意为之，
     * 因为屏上只有 ASCII 字体（见 lvgl-cjk-font-coverage）。 */
    rt_strncpy(g_app_cfg.weather_host, "api.seniverse.com", APP_CFG_HOST_LEN - 1);
    rt_strncpy(g_app_cfg.weather_path, "/v3/weather/now.json", APP_CFG_PATH_LEN - 1);
    rt_strncpy(g_app_cfg.weather_city, "beijing", APP_CFG_CITY_LEN - 1);

    /* 文字对话：OpenAI 兼容端点，全部留空让用户填 */
    g_app_cfg.llm_host[0] = '\0';
    g_app_cfg.llm_path[0] = '\0';
    g_app_cfg.llm_key[0]  = '\0';
    rt_strncpy(g_app_cfg.llm_model, "gpt-4o-mini", APP_CFG_MODEL_LEN - 1);

    g_app_cfg.dirty = RT_FALSE;
}

/* ===================== 极简 JSON ===================== */

/* 把字符串写入 out，处理 \" \\ \n \r \t 与 \u00XX 之外的通用转义。
 * 返回写入长度（不含结尾 0），越界时返回 -1。 */
static int json_escape(const char *in, char *out, rt_size_t cap)
{
    rt_size_t n = 0;

    while (*in)
    {
        unsigned char c = (unsigned char)*in;
        const char *rep = RT_NULL;
        char buf[8];

        if (c == '"')       rep = "\\\"";
        else if (c == '\\') rep = "\\\\";
        else if (c == '\n') rep = "\\n";
        else if (c == '\r') rep = "\\r";
        else if (c == '\t') rep = "\\t";
        else if (c < 0x20)
        {
            /* 其余控制字符一律转成 \u00XX，避免生成非法 JSON */
            rt_snprintf(buf, sizeof(buf), "\\u%04x", c);
            rep = buf;
        }

        if (rep != RT_NULL)
        {
            rt_size_t rl = rt_strlen(rep);
            if (n + rl >= cap) return -1;
            rt_memcpy(out + n, rep, rl);
            n += rl;
        }
        else
        {
            if (n + 1 >= cap) return -1;
            out[n++] = (char)c;
        }
        in++;
    }

    out[n] = '\0';
    return (int)n;
}

/* 找 JSON 里的 "key" 对应的字符串值。
 * 只处理顶层扁平对象；返回 0 表示找到并已解码到 out。 */
static int json_get_string(const char *json, const char *key,
                           char *out, rt_size_t cap)
{
    char pat[48];
    const char *p, *q;
    rt_size_t n = 0;

    if (json == RT_NULL || key == RT_NULL || cap == 0) return -1;

    rt_snprintf(pat, sizeof(pat), "\"%s\"", key);
    p = strstr(json, pat);
    if (p == RT_NULL) return -1;

    p += rt_strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return -1;
    p++;

    q = p;
    while (*q && *q != '"')
    {
        if (*q == '\\' && q[1]) q++;   /* 跳过被转义的字符 */
        q++;
    }
    if (*q != '"') return -1;

    /* 反向解码到 out */
    while (p < q)
    {
        char c = *p;

        if (c == '\\' && p + 1 < q)
        {
            p++;
            switch (*p)
            {
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'u':
            {
                /* \u00XX 只解 ASCII 范围，其余（中文等）转为 '?'。
                 * 本配置中不含中文，解码正确性优先于完备性。 */
                unsigned int v = 0;
                int i;

                if (p + 4 < q)
                {
                    for (i = 1; i <= 4; i++)
                    {
                        char h = p[i];
                        v <<= 4;
                        if (h >= '0' && h <= '9') v |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
                        else { v = '?'; break; }
                    }
                    p += 4;
                    c = (v < 0x80) ? (char)v : '?';
                }
                else
                {
                    c = '?';
                }
                break;
            }
            default: c = *p; break;
            }
        }

        if (n + 1 >= cap) return -1;    /* 超长：拒绝，不截断 */
        out[n++] = c;
        p++;
    }

    out[n] = '\0';
    return 0;
}

/* ===================== 落盘 ===================== */

static int cfg_write_file(const char *path)
{
    FILE *fp;
    rt_size_t i;
    int ret = 0;

    fp = fopen(path, "w");
    if (fp == RT_NULL)
    {
        LOG_W("open %s for write failed", path);
        return -RT_EIO;
    }

    /* fprintf 的返回值必须检查：卡写满或扇区出错时它会返回负数，
     * 而 FILE 缓冲中可能仍留有部分内容，最终 fclose 也可能"成功"。
     * 只检查 fclose 会得到一个字段缺失却被判定写入成功的 cfg.json，
     * 下次开机解析出的就是半套配置，后果比直接失败更严重。 */
    if (fprintf(fp, "{\n") < 0) { ret = -RT_EIO; goto __close; }

    for (i = 0; i < CFG_FIELD_NUM; i++)
    {
        /* 转义缓冲：源字段最大 96 字节，最坏膨胀 6 倍（全控制字符），
         * 1KB 静态缓冲放在栈上太贵，改用堆；写失败即整体失败。 */
        rt_size_t cap = (rt_size_t)s_fields[i].size * 6 + 8;
        char *esc = (char *)rt_malloc(cap);
        const char *val = cfg_slot(&s_fields[i]);

        if (esc == RT_NULL)
        {
            ret = -RT_ENOMEM;
            goto __close;
        }

        if (json_escape(val, esc, cap) < 0)
        {
            rt_free(esc);
            ret = -RT_EINVAL;
            goto __close;
        }

        if (fprintf(fp, "  \"%s\": \"%s\"%s\n",
                    s_fields[i].key, esc,
                    (i + 1 < CFG_FIELD_NUM) ? "," : "") < 0)
        {
            rt_free(esc);
            ret = -RT_EIO;
            goto __close;
        }
        rt_free(esc);
    }

    if (fprintf(fp, "}\n") < 0) ret = -RT_EIO;

__close:
    /* fclose 会把 stdio 缓冲刷到 elmfat；返回值同样要查 */
    if (fclose(fp) != 0 && ret == 0)
    {
        LOG_W("flush %s failed", path);
        ret = -RT_EIO;
    }

    if (ret != 0) LOG_W("write %s failed (%d)", path, ret);

    return ret;
}

/* 读取整个文件到堆缓冲。文件不存在返回 -1。 */
static int cfg_read_file(const char *path, char **out)
{
    FILE *fp;
    long sz;
    char *buf;

    *out = RT_NULL;

    fp = fopen(path, "r");
    if (fp == RT_NULL) return -1;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    sz = ftell(fp);
    if (sz <= 0 || sz > 4096)
    {
        /* 4KB 上限：14 个字段满载也才约 1.4KB，超了说明文件已损坏 */
        fclose(fp);
        return -1;
    }
    rewind(fp);

    buf = (char *)rt_malloc((rt_size_t)sz + 1);
    if (buf == RT_NULL) { fclose(fp); return -1; }

    if (fread(buf, 1, (rt_size_t)sz, fp) != (rt_size_t)sz)
    {
        rt_free(buf);
        fclose(fp);
        return -1;
    }
    buf[sz] = '\0';
    fclose(fp);

    *out = buf;
    return 0;
}

/* SD 是否可用：实际写入一次来判定，不依赖 stat 推测。
 *
 * 以下两个问题共同导致 2026-10-26 真机上 persist 始终为 no：
 *   1) 原先判断的是 /sdcard/wfizzz（固件自建的工作目录）。该目录在卡上
 *      原本不存在，首次插卡必然 stat 失败，误判 SD 不可用；而 cfg_reload
 *      又因 s_persist_ok 为假不肯创建它，形成死锁。
 *   2) 仅判断目录存在并不可靠：romfs 在 / 下提供了空的 sdcard 目录项，
 *      stat 可能成功，但该处为只读 romfs，无法写入。
 *
 * 因此此处直接执行 mkdir + 写探针文件 + 删除，全部成功才判定为可写。
 * 代价是开机多一次极小的 flash 写（几十字节），换取可靠的判断结果。 */
static rt_bool_t cfg_sd_ready(void)
{
    struct stat st;
    FILE *fp;

    /* 挂载点必须存在且是目录。未挂载时 dfs_filesystem_lookup 找不到
     * 对应文件系统，stat 返回 -ENOENT（dfs_file.c:580）。 */
    if (stat(APP_CFG_MNT, &st) != 0 || !S_ISDIR(st.st_mode))
        return RT_FALSE;

    /* 工作目录：不存在就建（elmfat 的 mkdir 不递归，这里只需一级） */
    if (stat(APP_CFG_DIR, &st) != 0 || !S_ISDIR(st.st_mode))
    {
        if (mkdir(APP_CFG_DIR, 0) != 0)
        {
            /* 可能被别人抢先建好，再确认一次 */
            if (stat(APP_CFG_DIR, &st) != 0 || !S_ISDIR(st.st_mode))
                return RT_FALSE;
        }
    }

    /* 可写性探针：写入一个字节后删除。
     * 卡被写保护、卡满或文件系统损坏都会在此暴露，而不必等到
     * 用户输入完密码才失败。
     * 使用独立的 .wtest 文件名，不复用 APP_CFG_FILE_TMP，因为后者是
     * 原子写入的中间文件，与真实保存并发时会相互干扰。 */
    fp = fopen(APP_CFG_FILE_PROBE, "w");
    if (fp == RT_NULL)
        return RT_FALSE;

    if (fputc('x', fp) == EOF)
    {
        fclose(fp);
        remove(APP_CFG_FILE_PROBE);
        return RT_FALSE;
    }

    if (fclose(fp) != 0)
    {
        remove(APP_CFG_FILE_PROBE);
        return RT_FALSE;
    }

    remove(APP_CFG_FILE_PROBE);
    return RT_TRUE;
}

/* 工作目录就位（幂等）。elmfat 的 mkdir 不递归，只建一级。 */
static void cfg_ensure_dir(void)
{
    struct stat st;

    if (stat(APP_CFG_DIR, &st) == 0 && S_ISDIR(st.st_mode))
        return;

    if (mkdir(APP_CFG_DIR, 0) != 0)
    {
        /* 目录可能已被别人建好（竞争），再确认一次 */
        if (stat(APP_CFG_DIR, &st) != 0 || !S_ISDIR(st.st_mode))
            LOG_W("mkdir %s failed", APP_CFG_DIR);
    }
}

static void cfg_lock(void)
{
    if (s_lock_ok) rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
}

static void cfg_unlock(void)
{
    if (s_lock_ok) rt_mutex_release(&s_lock);
}

int app_cfg_save(void)
{
    int ret;

    if (!s_persist_ok)
    {
        /* SD 不可用：标记 dirty，让网页能提示"未保存" */
        g_app_cfg.dirty = RT_TRUE;
        return -RT_EIO;
    }

    cfg_lock();

    cfg_ensure_dir();
    ret = cfg_write_file(APP_CFG_FILE_TMP);
    if (ret == 0)
    {
        /* 目标已存在时必须先删除。
         *
         * 原注释所述"elmfat 可覆盖已存在目标"有误：FatFs 的 f_rename()
         * 先执行 follow_path(newpath)，只要新名字已被占用即返回 FR_EXIST，
         * 并不覆盖（ff.c 的 f_rename 中为
         * `res = ... ? FR_NO_FILE : FR_EXIST`）。因此第二次及以后的保存
         * 全部失败，串口输出为反复的
         * `rename /sdcard/wfizzz/cfg.json.tmp -> /sdcard/wfizzz/cfg.json failed`；
         * 首次保存能成功仅因当时 cfg.json 尚不存在。
         *
         * 代价：删除目标到 rename 完成之间存在窗口。但 tmp 文件已写全并
         * fclose 过，掉电最坏结果为"卡上两个文件都在"或"只剩 tmp"，两者
         * 均可在下次开机时恢复（app_cfg_init 读不到 cfg.json 时使用默认值，
         * tmp 会被下一次保存覆盖）。这远优于"始终无法保存"。 */
        if (ret == 0)
        {
            struct stat st;

            /* 用 stat 而不是 access：DFS v1 没有实现 access()。
             * 目标不存在时 stat 返回非 0，直接跳过删除。 */
            if (stat(APP_CFG_FILE, &st) == 0)
            {
                if (remove(APP_CFG_FILE) != 0)
                {
                    LOG_W("remove old %s failed", APP_CFG_FILE);
                    remove(APP_CFG_FILE_TMP);
                    ret = -RT_EIO;
                }
            }
        }

        if (ret == 0 && rename(APP_CFG_FILE_TMP, APP_CFG_FILE) != 0)
        {
            LOG_W("rename %s -> %s failed", APP_CFG_FILE_TMP, APP_CFG_FILE);
            /* 清理半成品，避免下次误读 */
            remove(APP_CFG_FILE_TMP);
            ret = -RT_EIO;
        }
    }

    if (ret == 0) g_app_cfg.dirty = RT_FALSE;
    else          g_app_cfg.dirty = RT_TRUE;

    cfg_unlock();

    if (ret == 0) LOG_I("saved to %s", APP_CFG_FILE);
    return ret;
}

/* ===================== 对外接口 ===================== */

const char *app_cfg_get(const char *key)
{
    const struct cfg_field *f = cfg_find(key);

    if (f == RT_NULL) return RT_NULL;
    return cfg_slot(f);
}

rt_bool_t app_cfg_is_secret(const char *key)
{
    const struct cfg_field *f = cfg_find(key);

    return (f != RT_NULL) ? f->secret : RT_FALSE;
}

rt_bool_t app_cfg_persist_ok(void)
{
    return s_persist_ok;
}

rt_bool_t app_cfg_load_done(void)
{
    return s_load_done;
}

int app_cfg_set(const char *key, const char *value)
{
    const struct cfg_field *f = cfg_find(key);
    char *slot;
    rt_size_t len;

    if (f == RT_NULL || value == RT_NULL) return -RT_EINVAL;

    len = rt_strlen(value);
    if (len >= (rt_size_t)f->size)
    {
        LOG_W("value for \"%s\" too long (%u >= %u)", key,
              (unsigned)len, (unsigned)f->size);
        return -RT_EINVAL;
    }

    slot = cfg_slot(f);
    cfg_lock();
    if (rt_strcmp(slot, value) != 0)
    {
        rt_memcpy(slot, value, len + 1);
        g_app_cfg.dirty = RT_TRUE;
    }
    cfg_unlock();

    /* 落盘失败不影响返回值：值已在内存中生效，仅在重启后丢失。
     * 若将 -RT_EIO 返回给调用方，会被误读为"参数非法"；未插 SD 卡时
     * 屏上配网即会无论输入什么密码都报 invalid。保存能力由
     * app_cfg_dirty() 与 app_cfg_persist_ok() 单独查询。 */
    (void)app_cfg_save();

    return 0;
}

rt_bool_t app_cfg_dirty(void)
{
    return g_app_cfg.dirty;
}

/* ===================== 开机加载 ===================== */

void app_cfg_init(void)
{
    char *json = RT_NULL;
    rt_size_t i;
    rt_size_t applied = 0;

    if (s_inited) return;
    s_inited = RT_TRUE;

    /* 互斥量只初始化一次。
     *
     * 本函数会被重复调用：cfg_reload 会复位 s_inited 后再次调用，
     * app_cfg_boot_task 在开机那次未读到卡时也会再调一次。
     * 而对已在链表中的静态对象再次调用 rt_mutex_init()，会命中
     * rt_object_init() 的 `obj != object` 断言并直接停机。
     * 2026-10-26 真机即因此崩溃（SD 挂载成功后紧接着断言）。
     * 使用独立标志守卫，与 s_inited（管"是否已加载配置"）分开表达。 */
    if (!s_lock_inited)
    {
        s_lock_inited = RT_TRUE;
        if (rt_mutex_init(&s_lock, "appcfg", RT_IPC_FLAG_PRIO) == RT_EOK)
            s_lock_ok = RT_TRUE;
    }

    cfg_set_defaults();

    /* SD 挂载由 mnt.c 的热插拔线程异步完成，开机瞬间通常尚未就绪。
     *
     * 此处不等待：只探测一次，就绪则立即读取，未就绪则以 RAM-only 返回。
     * 不能在 INIT_APP_EXPORT 回调中 mdelay 等待 SD，因为该回调运行于
     * main 线程，阻塞 3 秒会将 LVGL 首屏与按键响应全部延后，表现为"开机卡住"。
     * 实际等待放在 app_cfg_boot_task 独立线程中（见文件末尾）。 */
    if (cfg_sd_ready())
    {
        s_persist_ok = RT_TRUE;
        /* cfg_sd_ready() 内部已确保工作目录存在；这里再调一次是幂等兜底，
         * 防止两次调用之间目录被外部删掉。 */
        cfg_ensure_dir();
    }

    if (!s_persist_ok)
    {
        /* 卡尚未挂载：配置仍为默认值，但读取本身"暂无结论"。
         * s_load_done 保持假，由 app_cfg_boot_task 在稍后读到或超时后置真。 */
        LOG_I("SD not ready yet, config is defaults for now");
        return;
    }

    if (cfg_read_file(APP_CFG_FILE, &json) != 0)
    {
        LOG_I("no %s yet, using defaults", APP_CFG_FILE);
        /* 主动落一份默认值，用户第一次打开网页就有完整骨架可改 */
        app_cfg_save();
        s_load_done = RT_TRUE;
        return;
    }

    for (i = 0; i < CFG_FIELD_NUM; i++)
    {
        char tmp[APP_CFG_KEY_LEN];

        if (json_get_string(json, s_fields[i].key, tmp, sizeof(tmp)) == 0)
        {
            /* 文件里的值同样要过长度校验；超长视为损坏，保留默认值 */
            if (rt_strlen(tmp) < (rt_size_t)s_fields[i].size)
            {
                rt_memcpy(cfg_slot(&s_fields[i]), tmp, rt_strlen(tmp) + 1);
                applied++;
            }
            else
            {
                LOG_W("field \"%s\" in file too long, ignored", s_fields[i].key);
            }
        }
    }

    rt_free(json);

    /* 日志只报数量，不报任何值 */
    LOG_I("loaded %u/%u fields from %s",
          (unsigned)applied, (unsigned)CFG_FIELD_NUM, APP_CFG_FILE);
    s_load_done = RT_TRUE;
}

/* ===================== msh ===================== */

/* 只打印键名与"是否已填"，不打印值。
 * 串口日志可能被截图或转发，凭据一旦出现在日志中即视为泄露。 */
static void cfg_dump(void)
{
    rt_size_t i;

    rt_kprintf("cfg file : %s\n", APP_CFG_FILE);
    rt_kprintf("persist  : %s\n", s_persist_ok ? "yes" : "no (RAM only)");
    rt_kprintf("dirty    : %s\n", g_app_cfg.dirty ? "yes" : "no");
    rt_kprintf("----------------------------------------\n");
    for (i = 0; i < CFG_FIELD_NUM; i++)
    {
        const char *v = cfg_slot(&s_fields[i]);
        const char *shown = (v[0] == '\0') ? "(empty)" : "(set)";

        rt_kprintf("%-14s : %s%s\n", s_fields[i].key, shown,
                   s_fields[i].secret ? "  [secret]" : "");
    }
}
MSH_CMD_EXPORT_ALIAS(cfg_dump, cfg, dump runtime config (no secret values));

/* 插卡后手动重试加载，避免必须重启 */
static void cfg_reload(void)
{
    if (!cfg_sd_ready())
    {
        rt_kprintf("%s not mounted\n", APP_CFG_MNT);
        return;
    }

    s_persist_ok = RT_TRUE;
    cfg_ensure_dir();

    s_inited = RT_FALSE;
    app_cfg_init();
    rt_kprintf("reloaded: persist=%s\n", s_persist_ok ? "yes" : "no");
}
MSH_CMD_EXPORT(cfg_reload, retry loading config from SD);

/* 开机尽早加载：webnet/CGI 与 UI 均依赖它。
 * 此处只执行一次不等待的尝试，不阻塞 main 线程。 */
static int app_cfg_auto_init(void)
{
    app_cfg_init();
    return RT_EOK;
}
INIT_APP_EXPORT(app_cfg_auto_init);

/* 后台补载线程：开机时 SD 通常尚未挂载完成（mnt.c 的热插拔线程每 1 秒
 * 轮询一次），因此此处异步等待最多 10 秒；一旦 /sdcard/wfizzz 出现即
 * 重新加载配置。该方式既不会阻塞开机，又能在卡稍晚就绪时读入配置。
 *
 * 优先级 27（低于 LVGL 21、app_data 24、墨墨 25）：本线程仅作补充加载，
 * 延迟可接受，不得抢占 UI 的 CPU。
 * 栈 2KB：调用链只有 stat/fopen/fread 与一个 128 字节的 tmp 缓冲。 */
#define CFG_BOOT_STACK   2048
#define CFG_BOOT_PRIO    27
#define CFG_BOOT_WAIT_MS 10000

static void app_cfg_boot_task(void *parameter)
{
    int waited = 0;

    RT_UNUSED(parameter);

    while (waited < CFG_BOOT_WAIT_MS)
    {
        if (s_persist_ok && s_inited)             /* 开机那次已读到 */
        {
            s_load_done = RT_TRUE;
            return;
        }

        if (cfg_sd_ready())
        {
            if (!s_persist_ok || !s_inited)
            {
                s_persist_ok = RT_TRUE;
                cfg_ensure_dir();
                s_inited = RT_FALSE;
                app_cfg_init();
                LOG_I("config picked up after %d ms", waited);
            }
            s_load_done = RT_TRUE;
            return;
        }

        rt_thread_mdelay(500);
        waited += 500;
    }

    /* 超时也没拿到卡：让等待方别再等下去。
     * 此后配置就是内存里的默认值，用户插卡后可跑 cfg_reload 重新读。 */
    s_load_done = RT_TRUE;
    LOG_W("no SD after %d ms: config stays RAM-only "
          "(plug card then run \"cfg_reload\")", waited);
}

static int app_cfg_boot_start(void)
{
    rt_thread_t tid;

    /* s_persist_ok 为真说明 app_cfg_init 那次已读到 SD 并加载完毕，
     * 不必再启动线程。判据是 s_persist_ok 而非"配置是否非默认"：
     * 首次开机时卡上本就没有 cfg.json，此时 s_persist_ok 同样为真。 */
    if (s_persist_ok) return RT_EOK;

    tid = rt_thread_create("cfgboot", app_cfg_boot_task, RT_NULL,
                           CFG_BOOT_STACK, CFG_BOOT_PRIO, 10);
    if (tid != RT_NULL) rt_thread_startup(tid);

    return RT_EOK;
}
/* 用 APP 级的最后一个导出位置起线程：此时 app_cfg_init 已跑过一遍 */
INIT_APP_EXPORT(app_cfg_boot_start);
