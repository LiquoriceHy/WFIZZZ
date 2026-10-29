# WFIZZZ

基于 Infineon PSoC™ Edge E84 双核开发板（Edgi-Talk 平台）的多功能智能终端固件，
运行 RT-Thread + LVGL。项目包含 CM33 与 CM55 两个独立工程，二者通过硬件 IPC 通信；
另有一个 ESP32-C6 端固件，作为 MQTT 局域网内的被控设备。

- **CM55（主）**：LVGL 界面、语音对话、鼾声 NPU 推理、WiFi 配网、MQTT
- **CM33**：传感器采集（AHT20 温湿度、ADC 电池电压）、跨核数据发布
- **ESP32-C6**：MQTT 被控设备端，接收命令、执行、回包，带 ST7789 状态屏

---

## 目录结构

```
WFIZZZ/
├── README.md                       本文件
├── LICENSE                         MIT 许可证（仅覆盖本项目的原创代码）
├── NOTICE                          第三方组件归属与许可证说明
├── .gitignore
├── .gitattributes
│
├── Edgi_Talk_M55_XiaoZhi/          CM55 主工程
│   ├── applications/               应用代码（日常修改区）
│   │   ├── main.c                  入口
│   │   ├── app_cfg.c/.h            运行时配置中枢（读写 SD 卡 cfg.json）
│   │   ├── app_data.c/.h           数据中枢（WiFi/RTC/内存/电池，供 UI 读取）
│   │   ├── data_store.c/.h         用户数据持久化（便签 + 闹钟共用）
│   │   ├── screen_saver.c/.h       自动息屏
│   │   ├── sd_mountpoint.c         提供 /sdcard 挂载点
│   │   ├── edge_shm.h              CM33↔CM55 共享内存布局（与 M33 侧逐字节一致）
│   │   ├── xz_app.c/.h             语音对话的应用层包装
│   │   ├── apps/                   抽屉应用：闹钟、便签、背单词、应用注册表
│   │   ├── audio/                  麦克风仲裁、音频事件检测、频谱
│   │   ├── net/                    配网网页、LAN 设备控制、天气、WiFi 自动连接
│   │   ├── snore/                  鼾声检测：推理后端、mel 前端、事件归档与汇总
│   │   ├── ui/                     LVGL 界面：仪表盘、系统页、抽屉、各应用页
│   │   └── xiaozhi/                语音对话客户端（含 iot/ 与 mcp/ 子目录）
│   ├── board/                      板级移植（board.c、linker_scripts/、Kconfig）
│   ├── sdcard/                     拷到 SD 卡的运行时资源
│   │   ├── README.md               拷卡步骤与配网说明
│   │   └── wfizzz/
│   │       ├── cfg.example.json    配置模板（复制为 cfg.json 后填写）
│   │       ├── cjk16.bin           16px 中文点阵字库（GB2312）
│   │       └── www/                配网网页前端（index/app.js/style.css）
│   ├── libraries/                  设备支持包与板级端口（Common/board/ports/ 含本地修改）
│   ├── libs/                       板级配置
│   ├── rt-thread/                  RT-Thread 5.0.2 内核
│   ├── packages/                   RT-Thread 软件包
│   ├── edge-impulse/               Edge Impulse SDK（鼾声推理依赖）
│   ├── resources/                  WHD 射频固件
│   ├── figures/                    界面截图
│   ├── docs/                       和风天气接入说明
│   ├── archive/                    已移除功能的头文件备份
│   ├── rtconfig.h / rtconfig.py    构建配置（rtconfig.h 由 Studio 生成，勿手改）
│   ├── Kconfig / SConstruct / SConscript
│   └── exclude_list.json           Studio 源文件排除清单
│
├── Edgi_Talk_M33_Driver_All/       CM33 工程（结构与 M55 相同）
│   ├── applications/
│   │   ├── main.c
│   │   ├── aht20_pub.c             AHT20 温湿度采集 + 跨核发布
│   │   ├── edge_shm.h              共享内存布局（与 M55 侧逐字节一致）
│   │   └── demo_*.c                各外设例程：ADC、AHT20、音频、Flash 测速、
│   │                               HyperRAM、按键、LSM6DS3、RTC、SD 卡
│   ├── board/  libraries/  libs/  rt-thread/  packages/
│   ├── config/                     启动配置
│   ├── figures/                    截图
│   └── build_secure_fw_studio.sh
│
├── ESP32_slave/                    ESP32-C6 被控设备端（ESP-IDF 6.0）
│   ├── CMakeLists.txt              工程入口
│   ├── sdkconfig.defaults          分区表、Flash 大小、LVGL 字体等配置
│   ├── dependencies.lock           托管组件版本锁定
│   ├── main/
│   │   ├── CMakeLists.txt          main 组件源文件与依赖声明
│   │   ├── idf_component.yml       托管组件依赖（mqtt/led_strip/cjson/lvgl）
│   │   ├── app_config.h            WiFi、MQTT、设备 id、引脚集中配置
│   │   ├── main.c                  入口与启动顺序
│   │   ├── wifi_sta.c/.h           WiFi STA 接入与断线重连
│   │   ├── mqtt_link.c/.h          MQTT 链路 + LWT 遗嘱 + 30s 心跳
│   │   ├── dev_cmd.c/.h            命令分发（ping / led / reboot）
│   │   ├── led_ctrl.c/.h           外接 LED 与板载 WS2812 镜像
│   │   └── status_ui.c/.h          ST7789 状态屏（LVGL）
│   └── driver/
│       ├── ST7789.c/.h             SPI 面板初始化
│       ├── LVGL_Driver.c/.h        LVGL 显示缓冲与刷新回调
│       └── Vernon_ST7789T/         Espressif 上游 ST7789 组件（Apache-2.0）
│
├── snore_check/                    鼾声模型验证与量化工具链
│   ├── README.md                   验证背景与结论
│   ├── results.md                  Pi 侧跑批结果
│   ├── snore_check.py              批量测试脚本
│   ├── model/
│   │   ├── lite-model_yamnet_tflite_1.tflite   YAMNet 原始 float32 模型
│   │   └── labels_yamnet.txt                   521 类标签
│   └── mcu/
│       ├── README.md               端侧部署全流程（量化 → 生成头 → 真机测试）
│       ├── quantize_real.py        int8 量化
│       ├── gen_headers.py          生成 C 数组头文件
│       ├── mcu_test.py             真机测试
│       ├── mel_check.py            mel 前端对齐校验
│       ├── yamnet_cal.tflite       量化后的 int8 模型
│       └── mcu_results.md          真机对比结果
│
└── tools/                          PC 侧辅助脚本
    ├── gen_npu_model.py            生成 Ethos-U55 模型 C 数组
    ├── npu_verify.py               推理结果校验
    ├── extract_inc.py
    ├── msh.py                      串口 msh 交互
    ├── watch.py                    串口监视
    ├── send_audio.py               音频下发
    ├── vela_pse84.ini              Vela 编译配置
    └── vela_out/                   Vela 编译产物
```

---

## 未包含的内容

Infineon 官方 Windows 工具链（`tools/device-configurator`、`tools/merge-hex`、
`tools/edgeprotecttools`）未收录。这三个工具与官方 BSP 包内完全一致，共约 162 MB，
编译本身不需要；如需修改设备配置或签名安全固件，请从
[RT-Thread Edgi-Talk BSP](https://github.com/RT-Thread-Studio/sdk-bsp-psoc_e84-edgi-talk)
获取对应版本。

---

## 编译

前提：RT-Thread Studio，以及 Edgi-Talk PSoC E84 板。

1. Studio 中 `File → Import → RT-Thread Studio Project`，选择 `Edgi_Talk_M55_XiaoZhi/`。
2. 确认 RT-Thread Settings 中的配置。Studio 以界面配置为准，修改 `.config`
   不会同步到 `.cproject`。
3. 编译，产物在 `Debug/`。

CM33 工程同样操作，目录为 `Edgi_Talk_M33_Driver_All/`。

**烧录顺序**：先 Secure M33，再 M33，最后 M55。

**ESP32-C6 端**用 ESP-IDF 6.0 构建：

```bash
cd ESP32_slave
idf.py set-target esp32c6
idf.py build flash monitor
```

托管组件（mqtt / led_strip / cjson / lvgl）由 `main/idf_component.yml` 声明、
`dependencies.lock` 锁定版本，首次构建时 IDF 自动下载到 `managed_components/`
（该目录不入库）。

---

## 配置

设备首次启动时无 WiFi 凭据，会自动开启热点：

- 热点 `WFIZZZ-Config`，口令 `wfizzz1234`
- 手机连上后浏览器访问 `http://192.168.169.1/`
- 也可在屏幕系统页点 WiFi 行，扫描列表 + 软键盘输入

配网后所有配置写入 SD 卡 `/sdcard/wfizzz/cfg.json`，包括 WiFi 凭据、语音服务端
地址、天气 API key 等。模板见
[`Edgi_Talk_M55_XiaoZhi/sdcard/wfizzz/cfg.example.json`](Edgi_Talk_M55_XiaoZhi/sdcard/wfizzz/cfg.example.json)。

> `cfg.json` 含明文凭据，已在 `.gitignore` 中排除，请勿提交。

MQTT broker 地址与凭据不在网页配置范围内，需在编译前修改：

| 文件 | 宏 |
|---|---|
| `Edgi_Talk_M55_XiaoZhi/applications/audio/audio_event.c` | `AE_MQTT_URI` / `AE_MQTT_USERNAME` / `AE_MQTT_PASSWORD` |
| `Edgi_Talk_M55_XiaoZhi/applications/net/lanctl_cfg.h` | `LANCTL_MQTT_URI` / `LANCTL_MQTT_USER` / `LANCTL_MQTT_PASS` |
| `Edgi_Talk_M55_XiaoZhi/applications/snore/snore_rt.c` | `SNORE_RT_MQTT_URI` / `SNORE_RT_MQTT_USER` / `SNORE_RT_MQTT_PASS` |
| `ESP32_slave/main/app_config.h` | `WIFI_SSID` / `WIFI_PASS` / `MQTT_URI` / `MQTT_USER` / `MQTT_PASS` |

仓库中这些位置均为空字符串。留空时相关模块打印警告并跳过网络连接，
其余功能（UI、鼾声检测、状态屏等）不受影响。

MQTT 凭据在两端必须一致，且 ESP32 侧 `DEV_ID` 要与 M55 的
`lanctl_cfg.h` 设备表中的 `id` 对应，否则命令无法路由。

---

## 许可证

本项目原创代码采用 MIT 许可证，见 [LICENSE](LICENSE)。
仓库内包含的第三方组件（RT-Thread、LVGL、CherryUSB、Edge Impulse SDK、
Infineon 设备支持包等）各自保留原有许可证，详见 [NOTICE](NOTICE)。
