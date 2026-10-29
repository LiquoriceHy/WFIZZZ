# 和风天气（QWeather）：已否决

日期：2026-10-26。本文是**否决记录**，不是接入方案。

> **结论：不要接和风天气。项目已定案改用心知天气（Seniverse）。**
> 选型依据见 `m55-weather-api-selection` 记忆与 `applications/weather.c` 的实施计划。
> 本文只保留"为什么否决"，以及万一将来要回头时需要知道的三个硬阻塞，避免重复踩坑。

---

## 否决依据：三个硬阻塞，全部实测

### 1. 旧公共 host 已全部失效

| Host | HTTP | 响应 |
|---|---|---|
| `devapi.qweather.com` | **403** | `{"error":{"status":403,"type":"...#invalid-host","title":"Invalid Host"}}` |
| `api.qweather.com` | **403** | 同上 |
| `geoapi.qweather.com` | **404** | 空体，路由已下线 |

**决定性证据：用错误 key 请求，返回的 403 报文与正确 key 一模一样。**
说明服务端在鉴权之前先校验 Host，旧 host 是被 Host 闸门堵死的，与 key 有效性无关。

新账号必须用控制台（`console.qweather.com/setting`）分配的专属 host
（形如 `<随机串>.re.qweatherapi.com`），官方称公共地址"从 2026 年起逐步停止服务"。
官方文档示例里的 `.xy.` 后缀已 NXDOMAIN，当前唯一有效后缀是 `.re.`。

### 2. 响应强制 gzip，无法关闭

三种请求方式的 body 前两字节**都是 `1f 8b`**：

| 请求头 | Content-Encoding | body |
|---|---|---|
| 不发 `Accept-Encoding` | `gzip` | `1f 8b ...` |
| `Accept-Encoding: identity` | `gzip` | `1f 8b ...` |
| `Accept-Encoding: gzip` | `gzip` | `1f 8b ...` |

服务端完全不看 `Accept-Encoding`，连 403 错误体都压缩。
而且 **`packages/webclient-v2.2.0` 源码里没有任何 gzip/inflate 字样**，
压缩体会原样交给应用层——不存在"打开某个开关就自动解压"的省事路径。
板上没有 `PKG_USING_ZLIB`，全树无 inflate 实现。

要接就得自己实现 RFC1951 inflate（几百行 C），或额外挂一个解压代理。
这是纯粹的额外成本，而心知天气根本不压缩。

### 3. 固件信任库没有对应的根证书

专属域名 `*.re.qweatherapi.com` 实测证书链：

```
叶证书    CN=*.re.qweatherapi.com
  ↑ 签发  CN=YR2            (Let's Encrypt)
  ↑ 签发  CN=Root YR         (ISRG)
  ↑ 签发  CN=ISRG Root X1    ← 根证书
```

而 `packages/mbedtls-v2.28.1/ports/src/tls_certificate.c` 的
`mbedtls_root_certificate[]` 是**全工程唯一** CA 来源（`tls_client.c:141` 只用这一份），
里面 3 张全是 DigiCert（Global Root G2 + 2 张中间），**没有 ISRG**。
（旧 host `devapi`/`api` 走的是 Sectigo R46/USERTrust，也匹配不上。）

mbedTLS 能力本身是齐的（`MBEDTLS_ECDSA_C`、`MBEDTLS_ECP_C`、
`MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED`、`MBEDTLS_SSL_PROTO_TLS1_2`、
`MBEDTLS_SSL_SERVER_NAME_INDICATION` 全开），纯粹是缺证书。

对比之下，心知天气 `api.seniverse.com` 的链是
`RapidSSL TLS RSA CA G1` ← **DigiCert Global Root G2**，正好是板内已嵌的根，
零改动就能验通。

---

## 如果将来真要回头接和风，需要知道的事

以下结论已经核实过，可直接用，不必重查：

**认证**：用 `X-QW-Api-Key` 请求头，不要 `?key=`（会进串口和代理日志）。两者不可同时用。
JWT 方式需要设备持 Ed25519 私钥，不适合我们。

**字段**（v7 全是 string，扁平好提取）：
- `/v7/weather/now` → `now.temp` / `now.feelsLike` / `now.text` / `now.icon` /
  `now.humidity` / `now.windDir` / `now.windScale` / `now.precip` / `now.pressure` / `now.vis`
- `/v7/weather/3d` → `daily[].fxDate` / `tempMax` / `tempMin` / `textDay` / `textNight` / `humidity`
- `/geo/v2/city/lookup` → `location[].id`（**字段名是 `id`，不是 `locationId`**）

**定位**：把 LocationID 写进配置（北京 `101010100`），不要运行时调 GeoAPI，
省一次请求也避开"不得批量缓存地理信息"的许可限制。

**错误处理**：和风的错误响应**不是** `{"code":"200"}` 那种结构，而是 RFC 7807，
**没有 `code` 字段**，只有 `error.status`。必须**先看 HTTP 状态码再看 body**。

**配额**：每月前 50000 次免费，QPM 3000，无日配额。
但 **2027-02-01 起 API Key 认证限 1000 次/天**（JWT 无限）。

**仅 HTTPS**，无明文端点（2016 年底就关了）。

**控制台坑**：「项目管理→凭据」里的 **IP 限制 / 网址限制必须留空**，
否则板子换网络会被 403 `security-restriction`。

**合规**：设备 UI 需注明"数据来源：和风天气"；禁止无限重试（会被当 DDoS 冻结账号）。

**响应体大小**：`now` 明文 492 B / gzip 343 B；`3d` 明文 1751 B / gzip 574 B。
解压缓冲 4 KB 够用。

---

## 附：本次调研的方法学教训

子代理最初的报告里断言"`.re.` 是唯一有效后缀"，并称它穷举了 676 个两字母后缀。
这类结论我在复核时只能验证到"`.re` 能解析、有 Let's Encrypt 通配证书"，
无法验证"其它后缀都不存在"。所以本文只保留我能独立复现的部分
（403 报文、gzip 行为、证书链、固件内证书清单），
穷举类结论不作为决策依据——反正选型已经由阻塞点 1、2 决定了。
