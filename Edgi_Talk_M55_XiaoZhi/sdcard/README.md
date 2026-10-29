# SD 卡种子内容（wfizzz）

本目录不是固件源码，**不会被 scons 编译**。它是要拷到 SD 卡上的运行时内容。

## 两种配网方式

改 WiFi 有两条路径，任选其一：

| 方式 | 怎么进 | 适用 |
|---|---|---|
| **屏上配网** | 系统页 → 点 **WiFi** 那一行 | 板子在手上，插着屏。不用电脑 |
| **网页配网** | 浏览器打开 `http://<板子IP>/`；未配置时板子起热点 `WFIZZZ-Config` / `wfizzz1234`，连上后开 `http://192.168.169.1/` | 板子已联网，或想改 key/模型名等其它配置 |

屏上配网界面：顶部有 **Rescan**（重新扫描）和 **Disconnect**（断开），
列表里点任意一个 SSID 会弹出密码键盘，输入后点 **Connect**。
密码框左边的 `AB` 按钮可以切换明文/密文显示。

> 屏上配网只处理 WiFi。API key、模型名这类长字符串仍然建议用网页填——
> 屏上没有中文输入法，长串敲起来太累。

## 目录结构

插上 SD 卡后，卡根目录应是这样：

```
/                                   ← SD 卡根
└── wfizzz/
    ├── www/                        ← 配网页静态资源
    │   ├── index.html
    │   ├── style.css
    │   └── app.js
    └── cfg.json                    ← 首次配网后由固件自动生成
```

对应固件里的路径常量（`applications/app_cfg.h`）：

| 宏 | 值 |
|---|---|
| `APP_CFG_DIR` | `/sdcard/wfizzz` |
| `APP_CFG_FILE` | `/sdcard/wfizzz/cfg.json` |
| `APP_CFG_WWW_DIR` | `/sdcard/wfizzz/www` |

## 怎么拷

### Windows 直接拷（推荐）

1. 把 SD 卡插到电脑，确认盘符（比如 `E:`）
2. 把本目录下的 `wfizzz` 整个文件夹拷到卡根：

```powershell
Copy-Item -Recurse .\sdcard\wfizzz E:\
```

3. 弹出卡，插回板子，重启。

> 卡的文件系统必须是 FAT32。固件用的是 elmfat，exFAT 需要额外配置才能挂载。

### 板上从串口拷

板子已经通过网线/WiFi 连到局域网时，也可以用 `tftp` 之类，但直接插电脑最省事。

## 不拷会怎样

固件会退化成"内嵌兜底页"模式：仍然能配网、能改配置，只是页面是编译进固件的精简版
（约 2.1KB，只有一个 WiFi 表单 + 状态）。这时候改样式必须重新烧固件——所以建议还是拷。

兜底页的触发条件是 `/sdcard/wfizzz/www/index.html` 打不开，串口会打印：

```
[I/app.webcfg] served built-in fallback page
```

## 修改页面后如何生效

直接用读卡器改 SD 卡上的文件，然后**按一下复位**即可，不需要重新烧固件。
（webnet 的根目录在 `webnet_init()` 时确定，文件内容每次请求都从卡上实时读，
所以严格说改完文件不重启也能看到变化；但浏览器可能缓存 CSS/JS，重启+强制刷新最稳妥。）

## 安全提醒

`cfg.json` 里存的是明文凭据（WiFi 密码、各类 API key）。这张卡等同于钥匙：

- 不要把卡借给别人
- 不要在公共电脑上打开 `cfg.json`
- 丢卡后应视为凭据已泄露，及时轮换所有 key
