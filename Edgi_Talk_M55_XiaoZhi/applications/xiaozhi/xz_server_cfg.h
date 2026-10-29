/*
 * xz_server_cfg.h - 小智服务端连接参数（从 app_cfg 读，带回退）
 *
 * 需要这一层的原因：
 *   xiaozhi.h 中 XIAOZHI_HOST、XIAOZHI_WSPATH、XIAOZHI_TOKEN 为编译期宏，
 *   每改一次都需重烧固件。而服务端地址是最需要变更的项目（官方云与
 *   自建服务端之间切换），因此改为运行时从 /sdcard/wfizzz/cfg.json 读取。
 *
 *   xiaozhi.cpp 为 C++，app_cfg 为 C 接口，本头文件以 extern "C" 包裹，
 *   两边均可直接 include。
 *
 * 回退策略：配置为空或非法时回退到官方云的默认值（与宏定义保持同一套），
 * 因此未曾配置 SD 卡的老设备行为完全不变。这是有意设计的：升级固件不应
 * 使原本可用的设备突然无法连接。
 */
#ifndef __XZ_SERVER_CFG_H__
#define __XZ_SERVER_CFG_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 连接参数 ---- */

/* WebSocket 语音通道参数（cfg.json 的 xiaozhi_host/port/path/token/ssl）。
 * host/path/token 返回内部指针，不要 free。 */
const char *xz_server_host(void);
const char *xz_server_path(void);
const char *xz_server_token(void);
rt_uint16_t xz_server_port(void);
rt_bool_t   xz_server_ssl(void);

/* OTA 握手接口地址。
 *
 * 两个端口的区别须注意：小智服务端默认 websocket 在 8000，
 * HTTP(OTA) 在 8003（见服务端 config.yaml 的 server.port 与 http_port）。
 * 因此 OTA 不能复用 ws 的端口，需单独配置一项。
 *
 * 留空时按"与 ws 相同 host 加 8003"推导，自建服务端只需填写 host 即可运行。 */
const char *xz_server_ota_port(void);

/* 拼出 OTA 请求的完整 URL（https/http 加 host 加 ota_port 加 /xiaozhi/ota/）。
 * out 长度建议不小于 128。返回 out。 */
char *xz_server_ota_url(char *out, rt_size_t len);

/* 把配置打出来（不含 token 明文），msh 命令 `xzcfg` 用。 */
void xz_server_dump(void);

#ifdef __cplusplus
}
#endif

#endif /* __XZ_SERVER_CFG_H__ */
