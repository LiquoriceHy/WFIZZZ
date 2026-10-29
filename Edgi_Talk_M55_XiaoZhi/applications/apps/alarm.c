/*
 * alarm.c - 闹钟管理（结构约定见 alarm.h / app_data.h）
 */
#include <rtthread.h>
#include "app_data.h"
#include "alarm.h"
#include "data_store.h"

#define AL_THREAD_STACK  1024
#define AL_THREAD_PRIO   28      /* 低于 app_data(24)，每秒一次的慢速比对 */
#define AL_PERIOD_MS     1000

/* 每槽"本分钟已触发过"标记（ts/60 分钟号），防同分钟反复触发；-1=开机未触发 */
static rt_int32_t last_fired_min[APP_ALARM_NUM] = {-1, -1, -1};

void app_alarm_save_request(void)
{
    data_store_mark_dirty();
}

/* 与 ui_dashboard 的 civil 换算一致：0=周日；只算周几，不依赖 libc */
static int alarm_weekday(rt_int64_t ts)
{
    long days = (long)(ts / 86400);
    int wd = (int)((days + 4) % 7);

    if (wd < 0)
        wd += 7;
    return wd;
}

/* 每秒比对：命中即触发（单次模式自动关槽），响铃中只管超时自动停 */
static void alarm_check(void)
{
    rt_int64_t ts;
    int minute_id, h, m;
    int i;

    if (!g_app.time_valid)
        return;

    ts = g_app.time_ts;

    /* 响铃中：只管超时自动停，不再比对新触发 */
    if (g_app.alarm_ring)
    {
        if (ts - g_app.alarm_ring_ts >= APP_ALARM_RING_TIMEOUT_S)
        {
            app_data_set_alarm_ring(RT_FALSE, 0);
            rt_kprintf("[alarm] ring timeout, auto stop\n");
        }
        return;
    }

    minute_id = (int)(ts / 60);
    h = (int)((ts / 3600) % 24);
    m = (int)((ts / 60) % 60);

    for (i = 0; i < APP_ALARM_NUM; i++)
    {
        app_data_alarm_t a = g_app.alarms[i];

        if (!a.enable || minute_id == last_fired_min[i])
            continue;
        if (a.hour != h || a.min != m)
            continue;

        /* 重复日位掩码：days==0 为单次（任意天响，触发即关）；
         * 否则今天（0=Sun..6=Sat 映射 bit6..bit0）必须选中才响 */
        if (a.days != 0 && !(a.days & APP_ALARM_DAY_BIT(alarm_weekday(ts))))
            continue;

        last_fired_min[i] = minute_id;

        /* 单次模式：触发即关闭该槽（走 setter，UI 下次打开就是关着的）。
         * 必须同时要求落盘：否则重启后这条已响过的单次闹钟会复活。 */
        if (a.days == 0)
        {
            a.enable = RT_FALSE;
            app_data_set_alarm(i, &a);
            app_alarm_save_request();
        }

        app_data_set_alarm_ring(RT_TRUE, ts);
        rt_kprintf("[alarm] ring! slot=%d %02d:%02d days=0x%02x\n",
                   i, a.hour, a.min, (unsigned int)a.days);
        break;   /* 一次只响一个 */
    }
}

void app_alarm_stop(void)
{
    app_data_set_alarm_ring(RT_FALSE, 0);
}

/* UI 保存某槽后调用：清"本分钟已触发"标记，把时间改成当前分钟也能响 */
void app_alarm_invalidate_slot(int idx)
{
    if (idx >= 0 && idx < APP_ALARM_NUM)
        last_fired_min[idx] = -1;
}

static void alarm_entry(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        alarm_check();
        rt_thread_mdelay(AL_PERIOD_MS);
    }
}

static int alarm_start(void)
{
    rt_thread_t tid;

    tid = rt_thread_create("alarm", alarm_entry, RT_NULL,
                           AL_THREAD_STACK, AL_THREAD_PRIO, 10);
    if (tid == RT_NULL)
    {
        rt_kprintf("[alarm] thread create failed\n");
        return -RT_ERROR;
    }
    rt_thread_startup(tid);
    return RT_EOK;
}
INIT_APP_EXPORT(alarm_start);
