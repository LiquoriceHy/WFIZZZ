/*
 * snore_server.c - 鼾声模型 TCP 测试服务器（配树莓派测试脚本用）
 *
 * 协议（都是主机端先发）：
 *   请求头 24B：'SNQ1' | mode(1B) | rsv(3B) | srate u32be | bits u32be |
 *               ch u32be | nsamp u32be
 *   随后 nsamp 个 int16 小端 PCM（16kHz 单声道，由 Pi 端 ffmpeg 转好）
 *   mode 0 = 推理：逐窗回文本行 "WIN <i> snore=<0.xxx> top=<idx>(<0.xxx>)"
 *            "fe=<ms> inf=<ms>"，末行 "DONE windows=<n> fe=<ms> inf=<ms>"
 *   mode 1 = mel 回显：逐窗回 96*64 个 float32 小端（Pi 侧比对前端）
 *   出错回 "ERR <msg>" 并断开
 *
 * 窗口规则与 Pi 参考脚本一致：15600 样本/窗、步 7800，不足一窗的尾巴丢弃。
 *
 * msh 命令：
 *   snore_server start|stop   启停服务器（端口 9000）
 *   snore_selftest            静音窗自检（不依赖树莓派）
 *   snore_info                查询 arena/量化参数
 */

#include <rtthread.h>
#include <string.h>
#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "snore_infer.h"
#include "snore_frontend.h"

#define SRV_PORT        9000
#define HDR_SIZE        24
#define RECV_BUF        4096
#define SRV_STACK       8192
/* 原 16KB。栈上最大局部量是 line[96]，收包缓冲 RECV_BUF(4KB) 与 s_wbuf/s_patch
 * 都在堆上；NPU 推理调用链会给 8KB 留约 20 倍余量。本线程仅 msh
 * `snore_server start` 时启动，不常驻。*/
#define SRV_PRIO        24

static volatile rt_bool_t s_running = RT_FALSE;
static int s_listen_fd = -1;
static rt_thread_t s_thread = RT_NULL;
/* 大缓冲放主堆：内部 SRAM .bss 只有 256KB，静态数组会把链接撑爆 */
static int16_t *s_wbuf = RT_NULL;               /* 15600 样本滑窗，31KB */
static float *s_patch = RT_NULL;                /* 96*64 mel patch，24KB */

static int recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    while (len > 0)
    {
        int n = recv(fd, p, (int)len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0)
    {
        int n = send(fd, p, (int)len, 0);
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 处理一路连接：收头 -> 流式收 PCM 滑窗 -> 按模式回包 */
static void handle_conn(int fd)
{
    uint8_t hdr[HDR_SIZE];
    uint32_t srate, bits, ch, nsamp;
    int mode;
    int filled = 0;                             /* wbuf 已填样本数 */
    rt_uint32_t win_idx = 0, fe_total = 0, inf_total = 0;
    uint8_t rx[RECV_BUF];

    if (s_wbuf == RT_NULL || s_patch == RT_NULL)
    {
        send_all(fd, "ERR server buffers not allocated\n", 33);
        return;
    }

    if (recv_all(fd, hdr, HDR_SIZE) != 0)
        return;
    if (memcmp(hdr, "SNQ1", 4) != 0)
    {
        send_all(fd, "ERR bad magic\n", 13);
        return;
    }
    mode = hdr[4];
    srate = ntohl(*(uint32_t *)(hdr + 8));
    bits = ntohl(*(uint32_t *)(hdr + 12));
    ch = ntohl(*(uint32_t *)(hdr + 16));
    nsamp = ntohl(*(uint32_t *)(hdr + 20));
    if (srate != 16000 || bits != 16 || ch != 1)
    {
        rt_kprintf("[snore] srv: bad fmt srate=%u bits=%u ch=%u\n",
                   (unsigned)srate, (unsigned)bits, (unsigned)ch);
        send_all(fd, "ERR bad format\n", 15);
        return;
    }
    rt_kprintf("[snore] srv: mode=%u nsamp=%u (%u.%us)\n", (unsigned)mode,
               (unsigned)nsamp, (unsigned)(nsamp / 16000),
               (unsigned)((nsamp % 16000) * 10 / 16000));

    while (nsamp > 0)
    {
        size_t want = RECV_BUF;
        int n, i;

        if (nsamp * 2 < want)
            want = nsamp * 2;
        if (recv_all(fd, rx, want) != 0)
        {
            rt_kprintf("[snore] srv: peer closed mid-file\n");
            return;
        }
        nsamp -= (uint32_t)(want / 2);
        n = (int)(want / 2);

        for (i = 0; i < n; i++)
        {
            s_wbuf[filled++] = (int16_t)(rx[i * 2] | (rx[i * 2 + 1] << 8));
            if (filled < SNORE_WIN_SAMPLES)
                continue;

            /* 攒满一窗，按模式处理 */
            if (mode == 1)
            {
                snore_frontend_window(s_wbuf, s_patch);
                if (send_all(fd, s_patch,
                             SNORE_MEL_FRAMES * SNORE_MEL_BANDS * sizeof(float)) != 0)
                    return;
            }
            else
            {
                snore_result_t r;
                char line[96];
                if (snore_infer_window(s_wbuf, &r) != 0)
                {
                    send_all(fd, "ERR infer failed\n", 17);
                    return;
                }
                fe_total += r.fe_ms;
                inf_total += r.infer_ms;
                rt_snprintf(line, sizeof(line),
                            "WIN %u snore=%u.%03u top=%u(%u.%03u) fe=%u inf=%u\n",
                            (unsigned)win_idx,
                            (unsigned)(r.snore * 1000) / 1000,
                            (unsigned)(r.snore * 1000) % 1000,
                            (unsigned)r.top_idx[0],
                            (unsigned)(r.top_score[0] * 1000) / 1000,
                            (unsigned)(r.top_score[0] * 1000) % 1000,
                            (unsigned)r.fe_ms, (unsigned)r.infer_ms);
                if (send_all(fd, line, strlen(line)) != 0)
                    return;
            }
            win_idx++;
            /* 步长 7800：保留后半窗，再收 7800 个样本 */
            memmove(s_wbuf, s_wbuf + SNORE_WIN_SAMPLES / 2,
                    (SNORE_WIN_SAMPLES / 2) * sizeof(int16_t));
            filled = SNORE_WIN_SAMPLES / 2;
        }
    }

    if (filled > 0)
        rt_kprintf("[snore] srv: tail %d samples dropped (<1 window)\n", filled);

    {
        char line[64];
        rt_snprintf(line, sizeof(line), "DONE windows=%u fe=%u inf=%u\n",
                    (unsigned)win_idx, (unsigned)fe_total, (unsigned)inf_total);
        send_all(fd, line, strlen(line));
    }
}

static void server_entry(void *param)
{
    struct sockaddr_in addr;
    int opt = 1;

    (void)param;
    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0)
    {
        rt_kprintf("[snore] srv: socket failed\n");
        s_running = RT_FALSE;
        return;
    }
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SRV_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s_listen_fd, 1) != 0)
    {
        rt_kprintf("[snore] srv: bind/listen failed\n");
        closesocket(s_listen_fd);
        s_listen_fd = -1;
        s_running = RT_FALSE;
        return;
    }
    rt_kprintf("[snore] srv listening on :%u\n", (unsigned)SRV_PORT);

    while (s_running)
    {
        int fd = accept(s_listen_fd, RT_NULL, RT_NULL);
        if (fd < 0)
            break;
        rt_kprintf("[snore] srv: client connected\n");
        handle_conn(fd);
        closesocket(fd);
        rt_kprintf("[snore] srv: client done\n");
    }

    if (s_listen_fd >= 0)
    {
        closesocket(s_listen_fd);
        s_listen_fd = -1;
    }
    s_running = RT_FALSE;
    rt_kprintf("[snore] srv stopped\n");
}

static int server_start(void)
{
    int rc;

    if (s_running)
    {
        rt_kprintf("[snore] srv already running\n");
        return 0;
    }
    rc = snore_infer_init();
    if (rc != 0 && rc != -4)
    {
        rt_kprintf("[snore] infer init failed (%d), server not started\n", rc);
        return -1;
    }
    if (s_wbuf == RT_NULL)
        s_wbuf = (int16_t *)rt_malloc(SNORE_WIN_SAMPLES * sizeof(int16_t));
    if (s_patch == RT_NULL)
        s_patch = (float *)rt_malloc(SNORE_MEL_FRAMES * SNORE_MEL_BANDS *
                                     sizeof(float));
    if (s_wbuf == RT_NULL || s_patch == RT_NULL)
    {
        rt_kprintf("[snore] srv buffer alloc failed\n");
        rt_free(s_wbuf); s_wbuf = RT_NULL;
        rt_free(s_patch); s_patch = RT_NULL;
        return -1;
    }
    s_running = RT_TRUE;
    s_thread = rt_thread_create("snore_srv", server_entry, RT_NULL,
                                SRV_STACK, SRV_PRIO, 20);
    if (s_thread == RT_NULL)
    {
        s_running = RT_FALSE;
        rt_kprintf("[snore] srv thread create failed\n");
        return -1;
    }
    rt_thread_startup(s_thread);
    return 0;
}

static void server_stop(void)
{
    if (!s_running)
        return;
    s_running = RT_FALSE;
    if (s_listen_fd >= 0)
    {
        shutdown(s_listen_fd, SHUT_RDWR);   /* 唤醒阻塞的 accept */
        closesocket(s_listen_fd);
        s_listen_fd = -1;
    }
}

static int snore_server(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "start") == 0)
        return server_start();
    if (argc >= 2 && strcmp(argv[1], "stop") == 0)
    {
        server_stop();
        return 0;
    }
    rt_kprintf("usage: snore_server start|stop\n");
    return 0;
}
MSH_CMD_EXPORT(snore_server, snore tcp test server: start|stop);

static void print_pct(const char *tag, float v)
{
    rt_uint32_t permille = (rt_uint32_t)(v * 1000.0f + 0.5f);
    rt_kprintf("  %s%u.%03u\n", tag, permille / 1000, permille % 1000);
}

static int snore_selftest(void)
{
    rt_int16_t *silence;
    snore_result_t r;
    int rc;

    rc = snore_infer_init();
    if (rc != 0 && rc != -4)
    {
        rt_kprintf("[snore] selftest: init failed (%d)\n", rc);
        return -1;
    }
    silence = (rt_int16_t *)rt_malloc(SNORE_WIN_SAMPLES * sizeof(int16_t));
    if (silence == RT_NULL)
    {
        rt_kprintf("[snore] selftest: buffer alloc failed\n");
        return -1;
    }
    memset(silence, 0, SNORE_WIN_SAMPLES * sizeof(int16_t));
    rc = snore_infer_window(silence, &r);
    rt_free(silence);
    if (rc != 0)
    {
        rt_kprintf("[snore] selftest: infer failed (%d)\n", rc);
        return -1;
    }
    rt_kprintf("[snore] selftest silence window:\n");
    print_pct("snore=", r.snore);
    rt_kprintf("  top1=class %u\n", (unsigned)r.top_idx[0]);
    print_pct("top1=", r.top_score[0]);
    rt_kprintf("  fe=%ums inf=%ums\n", (unsigned)r.fe_ms, (unsigned)r.infer_ms);
    rt_kprintf("[snore] selftest %s\n", r.snore < 0.1f ? "PASS" : "FAIL");
    return (r.snore < 0.1f) ? 0 : -2;
}
MSH_CMD_EXPORT(snore_selftest, snore model selftest on silence window);

static int snore_info(void)
{
    snore_info_t info;
    snore_infer_get_info(&info);
    if (!info.ok)
    {
        rt_kprintf("[snore] not initialized (run snore_selftest or snore_server start)\n");
        return 0;
    }
    rt_kprintf("[snore] arena %uKB @0x%08x (%s)\n",
               (unsigned)(info.arena_size / 1024), (unsigned)(uintptr_t)info.arena_addr,
               ((uintptr_t)info.arena_addr & 0xFF000000u) == 0x64000000u ? "HyperRAM" : "SRAM");
    rt_kprintf("[snore] in scale=%.6f zp=%d, out scale=%.6f zp=%d\n",
               info.in_scale, info.in_zp, info.out_scale, info.out_zp);
    return 0;
}
MSH_CMD_EXPORT(snore_info, snore arena and quantization info);

/* ---------------- 对外接口（UI 应用抽屉用） ---------------- */

/* 模型是否已初始化（arena 分配成功），UI 用它判断 Snore 应用是否可用 */
rt_bool_t snore_server_is_infer_ready(void)
{
    snore_info_t info;

    snore_infer_get_info(&info);
    return info.ok ? RT_TRUE : RT_FALSE;
}

/* TCP 测试服务器是否在监听 */
rt_bool_t snore_server_is_running(void)
{
    return s_running;
}

/* 模型 arena 信息文案（给抽屉显示） */
void snore_server_get_arena_text(char *buf, int len)
{
    snore_info_t info;

    if (buf == RT_NULL || len <= 0)
        return;

    snore_infer_get_info(&info);
    if (!info.ok)
    {
        rt_snprintf(buf, len, "model not loaded");
        return;
    }

    rt_snprintf(buf, len, "%uKB %s",
                (unsigned)(info.arena_size / 1024),
                ((uintptr_t)info.arena_addr & 0xFF000000u) == 0x64000000u
                    ? "HyperRAM" : "SRAM");
}
