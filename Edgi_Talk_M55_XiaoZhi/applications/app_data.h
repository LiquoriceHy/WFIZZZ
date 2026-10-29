/*
 * app_data.h - 智能终端数据中枢
 *
 * 一个低优先级线程每秒轮询"已启用"的数据源（WiFi/netdev/RTC/运行时长/
 * 内存/电池电压），结果写入 volatile 结构体 g_app；UI 用 lv_timer 每秒读取刷新（沿用
 * audio_meter 的 volatile 跨线程模式，字段级读写对仪表盘显示足够）。
 *
 * 外设数据一律走 setter：AHT20 温湿度由本文件线程的 ad_poll_temp_hum()
 * 从 M33 共享内存读出后调用（跨核发布，布局见 edge_shm.h）；MQTT/
 * 在线天气仍为预留接口，无人调用时字段保持初值，UI 显示占位
 * （"--"/灰色）。以后驱动写好后由驱动侧调用 setter 点亮 UI，无需改 UI。
 */
#ifndef __APP_DATA_H__
#define __APP_DATA_H__

#include <rtthread.h>

/* 墨墨背单词同步状态（maimemo.c 写入；非 OK 时数值无效，UI 显示占位） */
enum
{
    APP_DATA_MEMO_WAIT = 0,   /* 开机初值，尚未同步过 */
    APP_DATA_MEMO_OK,
    APP_DATA_MEMO_OFFLINE,    /* 未联网 */
    APP_DATA_MEMO_TOKEN,      /* token 失效（需要用户处理） */
    APP_DATA_MEMO_SYNC,       /* App 未开自动同步 / 当日未初始化 */
    APP_DATA_MEMO_ERROR,      /* 其它错误 */
};

typedef struct
{
    rt_uint8_t  state;        /* APP_DATA_MEMO_* */
    rt_uint16_t done;         /* 今日已完成 */
    rt_uint16_t total;        /* 今日目标 */
    rt_uint16_t known;        /* 认识（含熟知） */
    rt_uint16_t fuzzy;        /* 模糊 */
    rt_uint16_t forgot;       /* 忘记 */
} app_data_memo_t;

/* 闹钟（alarm.c 线程管理触发，ui_alarm 经 setter 修改；RAM-only，重启清空）
 * days 位掩码：bit0=周一 … bit6=周日（周一开头，Mon-Fri 恰为连续低位）。
 * days==0 是单次：任意天到点即响，触发后该槽自动关闭。
 * 文案解释（Once/Daily/Mon-Fri/Mo,We…）在 ui_alarm 的 alarm_days_text()。 */
#define APP_ALARM_DAY_BIT(wd)   ((rt_uint8_t)(1u << (((wd) + 6) % 7)))  /* wd: 0=Sun..6=Sat */
#define APP_ALARM_DAYS_ALL      0x7F    /* 每天 */
#define APP_ALARM_DAYS_WEEKDAYS 0x1F    /* 周一至五 */

#define APP_ALARM_NUM 3

typedef struct
{
    rt_uint8_t hour;          /* 0-23 */
    rt_uint8_t min;           /* 0-59 */
    rt_uint8_t days;          /* bit0=Mon..bit6=Sun；0=单次 */
    rt_bool_t  enable;
} app_data_alarm_t;

/* 小智聊天通道（xiaozhi_port.c 写入，app_registry 的 xz_draw 每 500ms 排空）
 *
 * 使用队列而非单槽：ui_app 每 500ms 才拉取一次，而小智的 TTS 会按
 * sentence_start 逐句推送。在单槽加 seq 的模型下，一个 tick 内到达两条
 * 消息就会永久丢失前一条，实测对话中表现为"跳句"。改为 4 槽环形队列后，
 * 写入方只追加，读取方一次排空，仅在一个 tick 内超过 4 条时才会丢失。
 *
 * 4 槽 × 1024 字节 = 4KB 静态内存。主堆当前余量约 40KB，这些静态开销
 * 比每条消息单独 malloc 更可预测。 */
#define APP_DATA_XZ_TEXT_LEN   1024   /* 单条消息（UTF-8，截断保护） */
#define APP_DATA_XZ_STATUS_LEN 48     /* 状态文案 */
#define APP_DATA_XZ_Q_LEN      4      /* 队列深度 */

typedef struct
{
    /* ---- 轮询源：本文件线程每秒刷新 ---- */
    rt_bool_t   wifi_connected;  /* wlan 框架已启用，实时读取 */
    rt_bool_t   wifi_valid;      /* 已连接时才可读 RSSI */
    int         wifi_rssi;       /* dBm，负值 */

    char        ip_str[16];      /* "--" 或 "192.168.1.123" */
    char        gw_str[16];      /* 网关，同上 */

    rt_uint32_t uptime_s;        /* 开机秒数 */
    rt_uint8_t  mem_used_pct;    /* 0-100 */
    rt_uint32_t mem_free_kb;
    rt_uint8_t  cpu_used_pct;    /* 0-100，cpu_usage 包每秒更新一次 */

    rt_bool_t   time_valid;      /* RTC 时间已设置（早于 2020 视为未设置） */
    rt_int64_t  time_ts;         /* 本地时间戳（已加时区），秒 */

    /* ---- 电池电压：本文件线程 ad_poll_batt() 约 30s 本地采样一次 ---- */
    rt_bool_t   batt_valid;      /* 首次成功采样后为真 */
    rt_uint16_t batt_mv;         /* 电池端电压 mV（分压比还原后） */
    rt_uint8_t  batt_pct;        /* 3300-4200mV 线性映射 0-100 */

    /* ---- 预留接口：当前无人写入，UI 显示占位 ---- */
    rt_bool_t   temp_hum_valid;  /* app_data_set_temp_hum() */
    float       temp_c;
    float       hum_pct;

    rt_bool_t   mqtt_online;     /* app_data_set_mqtt() */
    rt_bool_t   ntp_synced;      /* app_data_set_ntp_synced() */

    char        weather_text[24];/* app_data_set_weather()，空串=未接入 */

    /* ---- 墨墨背单词（maimemo.c 线程写入，UI 只读） ---- */
    app_data_memo_t memo;        /* state=APP_DATA_MEMO_OK 时数值有效 */

    /* ---- 闹钟（alarm.c 线程管理，ui_alarm 经 setter 修改） ---- */
    app_data_alarm_t alarms[APP_ALARM_NUM];   /* 主表：内存即真相，重启清空 */
    rt_bool_t   alarm_ring;      /* 响铃中；UI 点屏或 3 分钟超时后清除 */
    rt_int64_t  alarm_ring_ts;   /* 触发时刻（本地秒），遮罩显示与超时判断用 */

    /* ---- 小智聊天（xiaozhi_port.c 线程写入，app_registry 的 xz_draw 排空） ----
     *
     * 采用环形队列而非单槽：写入方 app_data_set_xiaozhi_msg() 将消息追加到
     * xiaozhi_q[head] 并推进 head；读取方 app_data_xz_pop() 从 tail 取出。
     * count 表示未读条数，读空时返回 RT_FALSE。
     *
     * 队列满时的策略是丢弃最旧的一条（tail 前进）并保留最新消息。对话场景下
     * 用户更关心刚说完的那句，而不是 2 秒前的消息。 */
    rt_uint32_t xiaozhi_seq;             /* 累计写入条数，仅用于调试/统计 */
    rt_uint8_t  xiaozhi_head;            /* 下一个写入位置 */
    rt_uint8_t  xiaozhi_tail;            /* 下一个读取位置 */
    rt_uint8_t  xiaozhi_count;           /* 未读条数 */
    rt_uint8_t  xiaozhi_role[APP_DATA_XZ_Q_LEN];
    char        xiaozhi_q[APP_DATA_XZ_Q_LEN][APP_DATA_XZ_TEXT_LEN];
    char        xiaozhi_status[APP_DATA_XZ_STATUS_LEN]; /* 空=未连 */
} app_data_t;

extern volatile app_data_t g_app;

/* 创建轮询线程（INIT_APP_EXPORT 自动调用，无需手动） */
int app_data_start(void);

/* ---- 预留 setter：将来由传感器/网络/MQTT 模块调用，UI 只读 ---- */
void app_data_set_temp_hum(float temp_c, float hum_pct);
void app_data_set_weather(const char *text);
void app_data_set_mqtt(rt_bool_t online);
void app_data_set_ntp_synced(void);
void app_data_set_memo(const app_data_memo_t *m);

/* 电池端电压（mV，ADC 本地采样），电量百分比在实现内换算 */
void app_data_set_batt(rt_uint16_t batt_mv);

/* ---- 闹钟 setter（alarm.c 线程与 ui_alarm 共用） ---- */
/* 整槽写入：先数值后 enable，读到 enable=1 时数值必已完整 */
void app_data_set_alarm(int idx, const app_data_alarm_t *a);
/* 响铃状态：on 时写入触发时刻，off 时清除（start_ts 忽略） */
void app_data_set_alarm_ring(rt_bool_t on, rt_int64_t start_ts);

/* ---- 小智聊天：写入一条消息（role 0=用户 1=小智），入队即返回 ---- */
void app_data_set_xiaozhi_msg(rt_uint8_t role, const char *text);
void app_data_set_xiaozhi_status(const char *text);

/* 取出一条未读消息。返回 RT_TRUE 表示取到了（role/text 已填），
 * RT_FALSE 表示队列空。UI 每个 tick 应循环调用直到返回 RT_FALSE，
 * 这样一次 tick 内到达的多条消息都不会丢。 */
rt_bool_t app_data_xz_pop(rt_uint8_t *role, char *buf, rt_size_t len);

/* 当前未读条数（UI 可用于显示"有新消息"）。 */
int app_data_xz_pending(void);

#endif
