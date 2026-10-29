#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""snore_check.py — 用预训练 YAMNet (AudioSet 521 类) 验证鼾声检测可行性。

用法 (树莓派, venv):
    ~/snore_venv/bin/python snore_check.py [audio_dir]

流程: ffmpeg 解码任意音频 -> 16kHz mono float32 -> 滑窗喂 TFLite ->
取 Snoring/Breathing/Speech/Music 类分数 -> 双阈值事件聚合 -> 汇总表。
支持两种模型输入: [1,15600] 波形 (tfhub yamnet/tflite/1) 和
[1,96,64,1] log-mel patch (classification 变体, 本地算 mel)。
"""

import glob
import os
import subprocess
import sys

import numpy as np

MODEL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "model",
                     "lite-model_yamnet_tflite_1.tflite")
LABELS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "model",
                      "labels_yamnet.txt")
SR = 16000
TRACK = ["Snoring", "Breathing", "Speech", "Music"]
TH_LO, TH_HI = 0.3, 0.5
MIN_RUN = 3            # 连续 >=3 个窗 (约 1.5~2s) 才算一次事件
YAM_MEL_BINS, YAM_PATCH_FRAMES = 64, 96


def load_interpreter(path):
    try:
        from ai_edge_litert.interpreter import Interpreter
        return Interpreter(model_path=path)
    except ImportError:
        pass
    try:
        from tflite_runtime.interpreter import Interpreter
        return Interpreter(model_path=path)
    except ImportError:
        pass
    from tensorflow.lite import Interpreter
    return Interpreter(model_path=path)


def load_labels():
    """兼容两种格式: TFJS 的 '"493": "Crunch",' 和纯名列表。"""
    import re
    pairs, plain = {}, []
    for line in open(LABELS, encoding="utf-8"):
        line = line.strip()
        if not line:
            continue
        m = re.match(r'^"?(\d+)"?\s*:\s*"([^"]+)"', line)
        if m:
            pairs[int(m.group(1))] = m.group(2)
        else:
            plain.append(line)
    if len(pairs) == 521:
        names = [pairs[i] for i in range(521)]
    elif len(plain) == 521:
        names = plain
    else:
        sys.exit("labels 文件异常: json=%d plain=%d, 需 521" % (len(pairs), len(plain)))
    if "Snoring" not in names:
        sys.exit("labels 缺少 Snoring 类")
    return names


def decode(path):
    """ffmpeg 解码为 16k mono float32 PCM。"""
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-ac", "1",
                        "-ar", str(SR), "-f", "f32le", "-"],
                       capture_output=True)
    if r.returncode != 0 or not r.stdout:
        raise RuntimeError("ffmpeg 解码失败: " + r.stderr.decode(errors="ignore")[-120:])
    return np.frombuffer(r.stdout, dtype=np.float32)


def yam_logmel(x):
    """复刻 YAMNet 特征: 25ms 窗/10ms 步 STFT 幅度 -> 64 mel (125~7500Hz) -> log(x+0.001)。"""
    def mel_mat(n_fft, sr, n_mels, fmin, fmax):
        def hz2mel(f):
            return 2595.0 * np.log10(1.0 + f / 700.0)
        def mel2hz(m):
            return 700.0 * (10.0 ** (m / 2595.0) - 1.0)
        mpts = mel2hz(hz2mel(fmin) + np.arange(n_mels + 2) *
                      (hz2mel(fmax) - hz2mel(fmin)) / (n_mels + 1))
        bins = np.floor((n_fft + 1) * mpts / sr).astype(int)
        fb = np.zeros((n_mels, n_fft // 2 + 1), dtype=np.float32)
        for i in range(n_mels):
            a, b, c = bins[i], bins[i + 1], bins[i + 2]
            if b > a:
                fb[i, a:b] = (np.arange(a, b) - a) / (b - a)
            if c > b:
                fb[i, b:c] = (c - np.arange(b, c)) / (c - b)
        return fb

    nfft, hop, win = 512, 160, 400
    fb = mel_mat(nfft, SR, YAM_MEL_BINS, 125.0, 7500.0)
    frames = 1 + (len(x) - win) // hop
    idx = np.arange(win)[None, :] + hop * np.arange(frames)[:, None]
    spec = np.abs(np.fft.rfft(x[idx] * np.hanning(win), n=nfft, axis=1)) ** 2
    mel = spec @ fb.T
    return np.log(mel + 0.001).astype(np.float32)


def run_model(interp, i_det, o_det, feed):
    interp.set_tensor(i_det["index"], feed)
    interp.invoke()
    out = interp.get_tensor(o_det["index"])
    return np.asarray(out, dtype=np.float32).reshape(-1, 521).mean(axis=0)


def pick_outputs(interp):
    o521 = [d for d in interp.get_output_details() if d["shape"][-1] == 521]
    if not o521:
        sys.exit("输出形状无 521: " + str([d["shape"].tolist() for d in interp.get_output_details()]))
    return o521[0]


def analyse(path, interp, i_det, o_det):
    x = decode(path)
    shape = i_det["shape"]
    scores = []
    if len(shape) <= 2:
        # 波形输入 (tfhub yamnet/tflite/1, 动态 rank-1, resize 成 15600=0.975s)
        win, hop = 15600, 7800
        for s in range(0, max(1, len(x) - win + 1), hop):
            seg = np.zeros(win, dtype=np.float32)
            seg[:min(win, len(x) - s)] = x[s:s + win]
            scores.append(run_model(interp, i_det, o_det,
                                    seg if len(shape) == 1 else seg[None, :]))
    elif len(shape) == 4:
        patch, hop = YAM_PATCH_FRAMES, YAM_PATCH_FRAMES // 2
        mel = yam_logmel(x)
        n = 1 + max(0, mel.shape[0] - patch)
        for s in range(0, n, hop):
            seg = np.zeros((patch, YAM_MEL_BINS), dtype=np.float32)
            seg[:min(patch, mel.shape[0] - s)] = mel[s:s + patch]
            scores.append(run_model(interp, i_det, o_det,
                                    seg[None, :, :, None]))
    else:
        sys.exit("不支持的输入形状: " + str(shape.tolist()))
    if not scores:
        raise RuntimeError("音频太短或解码为空")
    return np.stack(scores)   # [n_win, 521]


def events(sn, hop_s, th):
    """连续 >=MIN_RUN 个窗超阈值的段数与总时长(秒)。"""
    on = sn >= th
    n_ev, dur, run = 0, 0.0, 0
    for v in on:
        run = run + 1 if v else 0
        if run == MIN_RUN:
            n_ev += 1
        if run >= MIN_RUN:
            dur += hop_s
    return n_ev, dur


def gen_synth(audio_dir):
    import shlex
    synth = [("synth_silence.wav",
              "anullsrc=r=%d:cl=mono" % SR, 20.0),
             ("synth_pinknoise.wav",
              "anoisesrc=color=pink:r=%d:a=0.08:d=20" % SR, None)]
    for name, src, _ in synth:
        p = os.path.join(audio_dir, name)
        if os.path.exists(p):
            continue
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i", src,
                        "-t", "20", "-c:a", "pcm_s16le", p], check=True)


def main():
    audio_dir = sys.argv[1] if len(sys.argv) > 1 else "audio"
    labels = load_labels()
    idx = {t: labels.index(t) for t in TRACK}
    interp = load_interpreter(MODEL)
    if len(interp.get_input_details()[0]["shape"]) == 1:
        interp.resize_tensor_input(0, [15600])
    interp.allocate_tensors()
    i_det = interp.get_input_details()[0]
    o_det = pick_outputs(interp)
    print("input shape=%s dtype=%s  output=%s" %
          (i_det["shape"].tolist(), i_det["dtype"], o_det["shape"].tolist()))
    gen_synth(audio_dir)

    rows = []
    for f in sorted(glob.glob(os.path.join(audio_dir, "*"))):
        if f.endswith((".wav", ".mp3", ".flac", ".m4a", ".ogg")):
            name = os.path.basename(f)
            try:
                sc = analyse(f, interp, i_det, o_det)
            except Exception as e:
                print("SKIP %-34s %s" % (name, e))
                continue
            hop_s = 0.4875
            sn = sc[:, idx["Snoring"]]
            nlo, dlo = events(sn, hop_s, TH_LO)
            nhi, dhi = events(sn, hop_s, TH_HI)
            rows.append({
                "name": name,
                "group": "pos" if name.startswith(("pos", "snor")) else
                         ("synth" if name.startswith("synth") else name.split("_")[0]),
                "dur": len(sn) * hop_s,
                "sn_max": float(sn.max()), "sn_mean": float(sn.mean()),
                "ev_lo": nlo, "dur_lo": dlo, "ev_hi": nhi,
                "spk_max": float(sc[:, idx["Speech"]].max()),
                "brt_max": float(sc[:, idx["Breathing"]].max()),
                "mus_max": float(sc[:, idx["Music"]].max()),
            })
            r = rows[-1]
            print("%-34s %5.1fs  snore max=%.2f mean=%.2f  ev(%.1f)=%d/%.0fs ev(%.1f)=%d  spk=%.2f brt=%.2f mus=%.2f"
                  % (name, r["dur"], r["sn_max"], r["sn_mean"], TH_LO,
                     r["ev_lo"], r["dur_lo"], TH_HI, r["ev_hi"],
                     r["spk_max"], r["brt_max"], r["mus_max"]))

    if not rows:
        sys.exit("没有可处理音频")

    def grp(g):
        return [r for r in rows if r["group"] == g]

    lines = ["# YAMNet 鼾声检测验证结果", "",
             "模型: YAMNet TFLite (AudioSet 521 类), 输入 0.975s 波形窗 / 0.4875s 步长",
             "事件定义: 连续 %d 窗 Snoring 分数超阈值" % MIN_RUN, ""]
    for g in sorted(set(r["group"] for r in rows)):
        rs = grp(g)
        mx = max(r["sn_max"] for r in rs)
        fa_lo = sum(1 for r in rs if r["ev_lo"] > 0)
        lines.append("## %s (%d 个文件)" % (g, len(rs)))
        lines.append("")
        lines.append("| 文件 | 时长 | snore max | snore mean | ev@0.3 | 时长@0.3 | ev@0.5 | speech max |")
        lines.append("|---|---|---|---|---|---|---|---|")
        for r in rs:
            lines.append("| %s | %.0fs | %.3f | %.3f | %d | %.0fs | %d | %.3f |"
                         % (r["name"], r["dur"], r["sn_max"], r["sn_mean"],
                            r["ev_lo"], r["dur_lo"], r["ev_hi"], r["spk_max"]))
        lines.append("")
        lines.append("组内 snore 分数最大值 **%.3f**, 触发 ev@0.3 的文件 %d/%d"
                     % (mx, fa_lo, len(rs)))
        lines.append("")
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results.md")
    open(out, "w", encoding="utf-8").write("\n".join(lines))
    print("\n结果已写入", out)

    pos, neg = grp("pos"), grp("synth") + [r for r in rows if r["group"].startswith("neg")]
    if pos and neg:
        pmin = min(r["sn_max"] for r in pos)
        nmax = max(r["sn_max"] for r in neg)
        print("分离度: 正样本最低 max=%.3f vs 负样本最高 max=%.3f -> %s"
              % (pmin, nmax, "可分" if pmin > nmax else "有重叠, 需看逐文件"))


if __name__ == "__main__":
    main()
