/*
 * mic_arb.c - mic0 仲裁实现
 *
 * 设计取舍：仲裁器自身不持有 rt_device，只记录"谁有权使用"，设备的
 * open/close 仍由各使用者在本线程中执行。这样可避免句柄跨线程传递，
 * 也使"申请失败"能由调用方自然地作为错误处理。
 *
 * 抢占为协作式：仲裁器调用被抢占方的 stop 回调（该回调必须为异步，
 * 仅置标志位），然后轮询等待其调用 mic_arb_release()。这与 snore_rt
 * 现有的 s_stop 异步停止模型一致。
 */
#include <rtthread.h>
#include <string.h>

#include "mic_arb.h"

#define DBG_TAG "mic.arb"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* 优先级数字越大越优先 */
static int owner_prio(mic_arb_owner_t o)
{
    switch (o)
    {
    case MIC_ARB_XIAOZHI:     return 30;   /* 交互式，用户正在等 */
    case MIC_ARB_AUDIO_EVENT: return 20;   /* 手动测试 */
    case MIC_ARB_SNORE:       return 10;   /* 后台长跑 */
    default:                  return 0;
    }
}

static const char *owner_name(mic_arb_owner_t o)
{
    switch (o)
    {
    case MIC_ARB_SNORE:       return "snore";
    case MIC_ARB_AUDIO_EVENT: return "audio_event";
    case MIC_ARB_XIAOZHI:     return "xiaozhi";
    default:                  return "none";
    }
}

static volatile mic_arb_owner_t s_owner = MIC_ARB_NONE;
static void (*s_stop_cb[4])(void);

void mic_arb_set_stop_cb(mic_arb_owner_t owner, void (*cb)(void))
{
    if (owner > MIC_ARB_NONE && owner <= MIC_ARB_XIAOZHI)
        s_stop_cb[owner] = cb;
}

mic_arb_owner_t mic_arb_owner(void)
{
    return s_owner;
}

rt_err_t mic_arb_acquire(mic_arb_owner_t owner)
{
    int waited = 0;
    mic_arb_owner_t cur;

    if (owner == MIC_ARB_NONE)
        return -RT_EINVAL;

    for (;;)
    {
        cur = s_owner;

        if (cur == owner)
            return RT_EOK;              /* 幂等 */

        if (cur == MIC_ARB_NONE)
        {
            s_owner = owner;
            LOG_I("mic -> %s", owner_name(owner));
            return RT_EOK;
        }

        /* 被占：只在优先级更高时抢占，且只抢一次 */
        if (waited > 0 || owner_prio(owner) <= owner_prio(cur))
        {
            LOG_W("mic busy by %s, %s refused", owner_name(cur),
                  owner_name(owner));
            return -RT_EBUSY;
        }

        LOG_I("mic preempt: %s -> %s", owner_name(cur), owner_name(owner));

        if (s_stop_cb[cur] != RT_NULL)
            s_stop_cb[cur]();           /* 异步请求对方停 */
        else
            s_owner = MIC_ARB_NONE;     /* 没有回调的 owner 视为可立即让出 */

        /* 轮询等对方真正 release；给对方 3 秒（一窗推理约 0.34s） */
        while (s_owner != MIC_ARB_NONE && waited < 3000)
        {
            rt_thread_mdelay(10);
            waited += 10;
        }

        if (s_owner != MIC_ARB_NONE)
        {
            LOG_E("mic preempt timeout, %s still holds", owner_name(s_owner));
            return -RT_ETIMEOUT;
        }
        /* 回到循环顶部重试一次，成功后 waited>0 阻止二次抢占 */
    }
}

void mic_arb_release(mic_arb_owner_t owner)
{
    if (s_owner == owner)
    {
        s_owner = MIC_ARB_NONE;
        LOG_I("mic released by %s", owner_name(owner));
    }
}
