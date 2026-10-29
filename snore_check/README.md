# snore_check — YAMNet 鼾声检测可行性验证

零训练验证：用 AudioSet 预训练 YAMNet（521 类，含 Snoring）对鼾声/负样本打分。
2026-09-27 已在树莓派（Debian, aarch64, Debian 13, Python 3.13）跑通，结论见 `results.md`。

## 文件

- `snore_check.py` — 验证脚本（ffmpeg 解码 → 16k mono → 0.975s 窗/0.4875s 步推理 → 事件聚合）
- `results.md` — 树莓派跑批结果（同步自 `pi:snore_check/results.md`）
- `model/` — `lite-model_yamnet_tflite_1.tflite`（16MB，波形输入版）+ `labels_yamnet.txt`（521 类，TFJS 格式）
- `audio/` 不在 PC 侧；样本在 `pi:snore_check/audio/`（10 正 + 10 负 + 3 音乐，freesound hq 预览 mp3）

## 在树莓派上复跑

```bash
ssh pi
cd ~/snore_check && ~/snore_venv/bin/python snore_check.py   # venv: numpy + ai-edge-litert
```

新增测试音频：把 mp3/wav 丢进 `~/snore_check/audio/`，文件名前缀决定分组
（`pos_*` 鼾声 / `neg_fan|neg_ac|neg_speech|neg_music_*` 负样本 / `synth_*` 合成静音噪声）。

## 关键结论（详见 results.md）

- 分离度：正样本 snore max 最低 0.748，负样本最高 0.183 → 阈值取 0.3~0.5 均可零误报分开
- 单窗推理 7.3 ms（Pi 上），每秒只需 2 窗，算力余量巨大
- 最容易误触的是风扇声（neg max 0.18），仍是安全距离
- 下一步：端侧路线用 mel 输入版/int8（M55 + CMSIS-NN 或 NNP1），并用真实卧室录音调阈值
