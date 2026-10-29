# snore_check/mcu — M55 端侧鼾声检测工具链

模型部署到 Edgi Talk M55（Edgi_Talk_M55_XiaoZhi）+ 树莓派下发音频测试的全部工具。
日期：2026-10-01。

## 背景
- Pi 上的 float32 YAMNet（16MB 波形输入）放不进 M55 的 8MB XIP NOR → 用 mel 输入 int8 版。
- chayuto/yamnet-mel-int8-tflm 发布版是**死模型**（任何输入输出 ~0.5，合成随机校准 + by_name
  加载 h5 静默失败导致权重没挂上）。本目录用真实音频重新校准量化：`yamnet_cal.tflite`（4.05MB）。
- 主机侧验证（Pi，ai-edge-litert）：int8 vs float 参考 snore 分数 worst |Δ| = 0.038，
  量化域 [-6.89,+4.23] 与真实 mel 范围吻合。

## 文件
| 文件 | 作用 |
|---|---|
| yamnet_cal.tflite | 最终 int8 模型（真实校准，Snoring=class 38） |
| gen_headers.py | `--model x.tflite [audio...]`：验证 int8 vs float 分数 → 生成 snore_model_data.c/.h + snore_frontend_tables.h |
| quantize_real.py | 阿里云跑（TF 2.15.1）：frames 模型按序载 yamnet.h5 → 按层名搬到 mel-patch 模型 → 真实 patch 校准 PTQ |
| const_400.npy / const_16448.npy | 从 float 模型图内抽出的周期 Hann 窗[400] 和 mel 滤波器组[64,257]（前端 bit 级定稿勿重做） |
| features.py / params.py / yamnet.py | Google 官方前端与模型定义（yamnet.py 已加 tf_keras/tensorflow.keras 导入回退） |
| yamnet.h5 | Google 官方权重（15.3MB，GCS 直链在 Pi 上可下） |
| mcu_test.py | 硬件测试：23 样本 TCP 下发 → M55 逐窗分数 vs float 参考 → mcu_results.md |
| mel_check.py | 前端校验：M55 算的 96×64 mel patch vs numpy 逐元素比对 |
| calib_wav/ | 23 个样本的 16k mono 16bit wav（量化校准与测试共用） |

## M55 侧（applications/）
- `snore_frontend.c/.h` + `snore_frontend_tables.h`：mel 前端（400 窗/hop 160/FFT512/幅度谱/log(+0.001)）
- `snore_infer.cpp/.h` + `snore_model_data.c/.h`：TFLM 封装（arena 1536KB，复用 edge-impulse SDK 的 TFLM+CMSIS-NN）
- `snore_server.c`：TCP :9000 测试服务器；msh：`snore_server start|stop`、`snore_selftest`、`snore_info`
- `SConscript` 已加 `*.cpp` 收集

## 协议（snore_server.c）
24B 头：`'SNQ1' | mode u8 | rsv u8[3] | srate/bits/ch/nsamp 各 u32 BE`，随后 int16 LE PCM。
mode0=推理：逐窗回 `WIN <i> snore=x.xxx top=idx(0.xxx) fe=..ms inf=..ms`，末行 `DONE ...`；
mode1=mel 回显：逐窗 96×64 float32 LE。窗口 15600 步 7800，只处理整窗。

## 硬件测试流程（S3）
1. M55 上电连 WiFi 后 msh：`snore_selftest`（静音窗 snore 应 <0.1）→ `snore_server start` → `ifconfig` 记 IP
2. Pi：`~/snore_venv/bin/python mcu_test.py --host <IP>` → 生成 mcu_results.md
3. 先跑 `mel_check.py --host <IP> --file calib_wav/pos_180338.wav` 确认前端 max|Δ|<0.1
4. 验收：负样本 snore max ≤0.3、正样本 ≥0.6、0.3 事件一致率 ≥90%、单窗 ≤500ms

## 重量化流程（如需）
阿里云 /root/snore_bundle（/opt/py311 环境）：`SNORE_FLOAT_MODEL=... python3.11 quantize_real.py`。
Pi 无 TF（py3.13 无轮子、镜像 403、docker hub 不通）；TF 2.21/Keras3 与 tf-keras 老链路不兼容，须 2.15.x。
