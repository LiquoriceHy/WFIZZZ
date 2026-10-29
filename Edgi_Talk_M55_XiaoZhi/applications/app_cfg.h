/*
 * app_cfg.h - 运行时配置中枢（凭据与地址统一入口）
 *
 * 功能：
 *   集中保存可被网页修改的凭据、地址与模型名，替代散落在各处的 #define。
 *   源码中只保留占位默认值，真实值由用户在配网页填写后落到 SD 卡。
 *
 * 存储：
 *   /sdcard/wfizzz/cfg.json（elmfat，长文件名已开 RT_DFS_ELM_USE_LFN_3）。
 *   M55 不能写共享 NOR（擦写会打断 CM33 的 XIP 取指，2026-09-28 实测），
 *   /flash 归 M33 独占，SD 卡是 M55 唯一可写的持久化落点。
 *
 * 生命周期：
 *   app_cfg_init() 由 INIT_APP_EXPORT 在开机时调用一次：先填默认值，
 *   再尝试读 SD 覆盖。SD 未挂载或文件不存在时只降级不失败，
 *   persist_ok = RT_FALSE，UI 与网页可据此提示"配置不会保存"。
 *   网页写入走 app_cfg_set()，内部做白名单与长度校验后立即落盘。
 *
 * 约束与结论：
 *   - 运行时的凭据必须是 char[] 而非宏，宏无法在运行时修改。默认值仍集中
 *     在本文件，但默认值中不得写入真实凭据。
 *   - 日志只输出键名，不输出值（rt_kprintf 为明文串口输出）。
 *   - 写入采用 tmp + rename 两步，避免掉电写坏主文件。
 *   - 目录 /sdcard/wfizzz 不会自动存在，app_cfg_init 中需要 mkdir。
 *   - 2026-10-26 实测主堆峰值已达 92.3%（496KB/525KB）。本模块的
 *     JSON 文本缓冲走 rt_malloc 且用完即释放，不设静态大数组。
 */
#ifndef __APP_CFG_H__
#define __APP_CFG_H__

#include <rtthread.h>

/* 字段长度：含结尾 '\0'。网页表单的上限必须与这里一致 */
#define APP_CFG_SSID_LEN     33
#define APP_CFG_PASS_LEN     65
#define APP_CFG_HOST_LEN     64
#define APP_CFG_PATH_LEN     96
#define APP_CFG_TOKEN_LEN    96
#define APP_CFG_KEY_LEN      96
#define APP_CFG_MODEL_LEN    48
#define APP_CFG_CITY_LEN     24
/* 端口用字符串存储：本模块的 JSON 只处理扁平字符串（见 app_cfg.c 头注释），
 * 为两个数字单独实现一套整数解析不划算。8 字节可容纳 "65535" 与 '\0'。 */
#define APP_CFG_PORT_LEN     8
/* 布尔值用 "0"/"1" 字符串，理由同上 */
#define APP_CFG_FLAG_LEN     4

/* 配置文件与目录
 *
 * APP_CFG_MNT 是挂载点，APP_CFG_DIR 是挂载点下的工作目录。
 * 两者必须区分：判断"SD 是否可用"只能依据挂载点，不能依据工作目录。
 * 2026-10-26 的问题：原先直接用 stat("/sdcard/wfizzz") 判断 SD 可用性，
 * 但该子目录在卡上原本不存在（由固件创建），于是首次插卡必然 stat 失败，
 * 进而判定"SD 不可用"，导致配置始终为 RAM-only、cfg_reload 始终报
 * "SD still not mounted"，而实际卡已正常挂载。 */
#define APP_CFG_MNT          "/sdcard"
#define APP_CFG_DIR          APP_CFG_MNT "/wfizzz"
#define APP_CFG_FILE         APP_CFG_DIR "/cfg.json"
#define APP_CFG_FILE_TMP     APP_CFG_DIR "/cfg.json.tmp"
#define APP_CFG_FILE_PROBE   APP_CFG_DIR "/.wtest"   /* 可写性探针，见 cfg_sd_ready() */
#define APP_CFG_WWW_DIR      APP_CFG_DIR "/www"

typedef struct
{
    /* WiFi（CYW55500 仅 2.4GHz；驱动固定 WPA2-AES-PSK，见 whd_wlan.c:249） */
    char wifi_ssid[APP_CFG_SSID_LEN];
    char wifi_pass[APP_CFG_PASS_LEN];

    /* 小智语音服务
     *
     * 这五项决定设备连接的服务端。默认值为官方云（api.tenclass.net，
     * wss + 443）；改用树莓派自建服务端时只需修改前四项，并将 ssl 设为 0。
     * 修改后无需重烧固件，可通过网页配置或直接修改 cfg.json 生效。 */
    char xiaozhi_host[APP_CFG_HOST_LEN];
    char xiaozhi_path[APP_CFG_PATH_LEN];
    char xiaozhi_token[APP_CFG_TOKEN_LEN];
    char xiaozhi_port[APP_CFG_PORT_LEN];      /* ws 端口，ASCII 十进制 */
    char xiaozhi_ota_port[APP_CFG_PORT_LEN];  /* OTA(HTTP) 端口，常与 ws 不同 */
    char xiaozhi_ssl[APP_CFG_FLAG_LEN];       /* "1"=wss/https "0"=ws/http */

    /* 墨墨背单词 */
    char memo_host[APP_CFG_HOST_LEN];
    char memo_token[APP_CFG_TOKEN_LEN];

    /* 天气（P2 使用，心知天气 Seniverse）
     *
     * weather_city 与 weather_path 分开存储：weather_path 只存纯路径
     * （如 /v3/weather/now.json），city、language、unit 由 weather.c 在
     * 拼接 URL 时追加。原因是完整 URL 含 key 接近 90 字符，会占满
     * APP_CFG_PATH_LEN(96)；且更换城市只需修改 city，无需重写整条路径。 */
    char weather_host[APP_CFG_HOST_LEN];
    char weather_path[APP_CFG_PATH_LEN];
    char weather_key[APP_CFG_KEY_LEN];
    char weather_city[APP_CFG_CITY_LEN];

    /* 板上文字对话（P4 使用，OpenAI 兼容 /v1/chat/completions） */
    char llm_host[APP_CFG_HOST_LEN];
    char llm_path[APP_CFG_PATH_LEN];
    char llm_key[APP_CFG_KEY_LEN];
    char llm_model[APP_CFG_MODEL_LEN];

    /* ---- 运行时状态，不写入 JSON ---- */
    rt_bool_t dirty;        /* 有改动但未能落盘（SD 不可用） */
} app_cfg_t;

extern app_cfg_t g_app_cfg;

/* 开机加载：填默认值，确保目录存在，读 SD 覆盖。永不失败。 */
void app_cfg_init(void);

/* SD 是否可写（初始化后有效）。RT_FALSE 时改配置只在 RAM 生效。 */
rt_bool_t app_cfg_persist_ok(void);

/* 开机那次从 SD 读取是否已有结论（读到了，或确认卡上没有、未插卡）。
 *
 * 与 app_cfg_persist_ok() 的区别如下：
 *   persist_ok 只说明"SD 可用"。开机瞬间卡尚未挂载时它同样为 RT_FALSE，
 *   而此时配置中全是默认值（wifi_ssid 为空）。
 *   任何"看到 wifi_ssid 为空即视为未配网"的逻辑（如 web_cfg 启动 SoftAP）
 *   必须先检查本函数：SD 尚未读完时 wifi_ssid 为空只表示"还没读到"，
 *   不表示"用户未配"。缺少此判断会在已有凭据的机器上误启 SoftAP。
 *
 * app_cfg_boot_task 最多等待 SD 10 秒，之后无论如何置真。 */
rt_bool_t app_cfg_load_done(void);

/* 按 key 设置一个字段（自动落盘）。
 *
 * 返回值语义于 2026-10-26 修正，须注意区分：
 *   返回 0        = 值合法，已写入内存，并在 SD 可用时已落盘。
 *   返回 <0       = 值本身非法（未知 key 或超长）。
 *
 * 关键点：落盘失败不算失败。SD 卡不在或无法写入时，本函数仍返回 0，
 * 值已在内存中生效，仅重启后丢失；该状态通过 app_cfg_dirty() 查询，
 * 不应当作参数错误处理。
 *
 * 修改原因：原先本函数直接返回 app_cfg_save() 的结果，而 app_cfg_save()
 * 在 SD 不可用时返回 -RT_EIO；调用方（web_cfg_connect_begin 与屏上配网）
 * 又将该负数解释为"SSID/密码非法"，导致未插 SD 卡时无论输入什么密码
 * 都报 invalid。保存能力与参数合法性是两件事，必须分开表达。 */
int app_cfg_set(const char *key, const char *value);

/* 是否有改动尚未落盘（SD 不可用或上次保存失败）。UI 可据此提示
 * "配置仅在本次运行有效"。 */
rt_bool_t app_cfg_dirty(void);

/* 取字段当前值（RT_NULL 表示未知 key）。用于网页回显。 */
const char *app_cfg_get(const char *key);

/* 把所有字段写回 SD。返回 0 成功，负数失败。网页 POST 可显式调用。 */
int app_cfg_save(void);

/* 是否为敏感字段（网页回显时掩码为 ****后4位） */
rt_bool_t app_cfg_is_secret(const char *key);

/* JSON 键名：网页、解析、写入三处共用，避免拼写不一致 */
#define APP_CFG_KEY_WIFI_SSID   "wifi_ssid"
#define APP_CFG_KEY_WIFI_PASS   "wifi_pass"
#define APP_CFG_KEY_XZ_HOST     "xiaozhi_host"
#define APP_CFG_KEY_XZ_PATH     "xiaozhi_path"
#define APP_CFG_KEY_XZ_TOKEN    "xiaozhi_token"
#define APP_CFG_KEY_XZ_PORT     "xiaozhi_port"
#define APP_CFG_KEY_XZ_OTA_PORT "xiaozhi_ota_port"
#define APP_CFG_KEY_XZ_SSL      "xiaozhi_ssl"
#define APP_CFG_KEY_MEMO_HOST   "memo_host"
#define APP_CFG_KEY_MEMO_TOKEN  "memo_token"
#define APP_CFG_KEY_WEA_HOST    "weather_host"
#define APP_CFG_KEY_WEA_PATH    "weather_path"
#define APP_CFG_KEY_WEA_KEY     "weather_key"
#define APP_CFG_KEY_WEA_CITY    "weather_city"
#define APP_CFG_KEY_LLM_HOST    "llm_host"
#define APP_CFG_KEY_LLM_PATH    "llm_path"
#define APP_CFG_KEY_LLM_KEY     "llm_key"
#define APP_CFG_KEY_LLM_MODEL   "llm_model"

#endif /* __APP_CFG_H__ */
