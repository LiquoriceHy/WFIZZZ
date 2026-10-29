/*
 * web_cfg.h - 板载配置网页（webnet + SD 卡静态资源）
 *
 * 用途：
 *   STA 已连接时，局域网内任何浏览器访问 http://<板子IP>/ 即可修改
 *   WiFi 凭据、各类 API key 与模型名。STA 未配置或连接失败时，工作线程
 *   启动 SoftAP（SSID=WFIZZZ-xxxx），手机连接后访问 http://192.168.169.1/
 *   进行首次配网。
 *
 * 安全边界：
 *   1) 明文 HTTP，无登录鉴权。设计前提是仅在受信任的局域网或 AP 内使用。
 *      暴露到公网等同于凭据直接泄露，不要做端口映射。
 *   2) 敏感字段回显时只给出 "****" 加后 4 位，页面拿到的是掩码；
 *      提交空值表示"不修改"，仅填了新值才覆盖。这样用户无需为了修改
 *      一个字段而重新抄写其它密钥。
 *   3) 串口日志只打印键名，不打印值。
 */
#ifndef __WEB_CFG_H__
#define __WEB_CFG_H__

#include <rtthread.h>

/* 网页根目录（SD 卡）。文件不存在时 CGI 使用内嵌的兜底页面，
 * 因此不插卡也能配网，只是页面较为简单。 */
#define WEB_CFG_ROOT        APP_CFG_WWW_DIR

/* SoftAP 参数。密码至少 8 位，否则 whd 会拒绝（WPA2 规范）。 */
#define WEB_CFG_AP_SSID     "WFIZZZ-Config"
#define WEB_CFG_AP_PASS     "wfizzz1234"
#define WEB_CFG_AP_CHANNEL  6

/* 启动配置网页服务（幂等）。
 * 返回 0 成功；负数表示失败（webnet 线程创建失败）。 */
int web_cfg_start(void);

/* 当前是否已有可达的 Web UI。
 * RT_TRUE = STA 已连上，或 AP 已启动，二者任一即可。 */
rt_bool_t web_cfg_is_serving(void);

/* 供其它模块（如状态栏）查配网态：
 * 0=未配网 1=STA 连接中 2=STA 已连 3=AP 模式 */
int web_cfg_get_state(void);

/* ===================== 屏上配网界面用的接口 =====================
 *
 * 设计原则：LVGL 线程不得阻塞。扫描需要 2-6 秒、连接需要 3-10 秒，
 * 都不能在 UI 线程内等待，否则整屏卡死、触摸无响应，看门狗也可能触发。
 * 因此拆分为"发命令 + 轮询快照"两步：
 *   web_cfg_scan_begin()     立即返回，由后台线程执行扫描
 *   web_cfg_scan_snapshot()  由 UI 每 500ms 轮询，获取就绪状态、数量与条目
 *   web_cfg_connect_begin()  立即返回，由状态机线程执行连接
 *   web_cfg_get_state()      轮询连接结果
 *
 * 一次扫描最多记录 WEB_CFG_SCAN_MAX 个 AP（2.4GHz 常见环境中为 10-20 个，
 * 上限取 20：每个条目 40 字节，静态数组共 800 字节，可以接受）。
 */

#define WEB_CFG_SCAN_MAX      20
#define WEB_CFG_SSID_MAX      33      /* 32 + '\0' */

/* 扫描状态机：IDLE=未扫描 RUNNING=扫描中 DONE=有结果 FAIL=失败 */
enum
{
    WEB_CFG_SCAN_IDLE = 0,
    WEB_CFG_SCAN_RUNNING,
    WEB_CFG_SCAN_DONE,
    WEB_CFG_SCAN_FAIL,
};

typedef struct
{
    char ssid[WEB_CFG_SSID_MAX];
    rt_int16_t rssi;
    rt_int16_t channel;
    rt_bool_t  secure;
} web_cfg_ap_t;

/* 发起一次扫描（异步，立即返回）。已在扫描中则直接返回 RT_EBUSY。
 * 返回 0 表示已提交。 */
int web_cfg_scan_begin(void);

/* 读扫描快照。返回值是上面 WEB_CFG_SCAN_* 之一。
 * 仅当返回 WEB_CFG_SCAN_DONE 时 *count 与 ap[] 有效。 */
int web_cfg_scan_snapshot(web_cfg_ap_t *ap, int max, int *count);

/* 用给定的 SSID/密码发起连接（异步，立即返回，同时保存到 SD 配置）。
 * 返回 0 表示已提交；-RT_EINVAL 表示参数非法。 */
int web_cfg_connect_begin(const char *ssid, const char *pass);

/* 断开当前 STA 连接（异步）。供 UI 的"断开"按钮使用。 */
void web_cfg_disconnect(void);

#endif /* __WEB_CFG_H__ */
