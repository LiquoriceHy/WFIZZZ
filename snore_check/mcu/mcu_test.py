#!/usr/bin/env python3
"""Send audio to the M55 snore server over TCP, collect per-window scores,
compare against the float YAMNet reference on the same windows.

Usage:  ~/snore_venv/bin/python mcu_test.py --host <board-ip> [--port 9000]
        [--out mcu_results.md] [audio files...]

Protocol (see snore_server.c): 24B header 'SNQ1' mode rsv srate/be bits/be
ch/be nsamp/be, then nsamp*int16 LE PCM. MCU replies WIN/DONE lines.
"""
import glob
import os
import socket
import subprocess
import sys
import threading
import time

import numpy as np

SR = 16000
WIN = 15600
HOP = 7800
SNORE_IDX = 38
AUDIO_DIR = '/home/user/snore_check/audio'
FLOAT_MODEL = '/home/user/snore_check/model/lite-model_yamnet_tflite_1.tflite'
DEFAULT_OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'mcu_results.md')


def decode_i16(path):
    """-> int16 bytes, 16k mono (what the MCU consumes)."""
    return subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', path, '-ac', '1', '-ar', str(SR),
         '-f', 's16le', '-'], capture_output=True, check=True).stdout


def decode_f32(path):
    """-> float32 [-1,1], 16k mono (what the float reference consumes)."""
    return np.frombuffer(subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', path, '-ac', '1', '-ar', str(SR),
         '-f', 'f32le', '-'], capture_output=True, check=True).stdout,
        dtype=np.float32)


def windows(x):
    n = 1 + (len(x) - WIN) // HOP if len(x) >= WIN else 0
    return [x[i * HOP:i * HOP + WIN] for i in range(n)]


def send_file(host, port, path, mode=0):
    """Stream one file; return list of parsed result dicts."""
    pcm = decode_i16(path)
    nsamp = len(pcm) // 2
    hdr = b'SNQ1' + bytes([mode, 0, 0, 0]) + \
        SR.to_bytes(4, 'big') + (16).to_bytes(4, 'big') + \
        (1).to_bytes(4, 'big') + nsamp.to_bytes(4, 'big')

    s = socket.create_connection((host, port), timeout=30)
    results = []
    stop_reader = threading.Event()

    def reader():
        buf = b''
        while not stop_reader.is_set():
            try:
                chunk = s.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                txt = line.decode('ascii', 'replace').strip()
                if txt.startswith('WIN'):
                    d = {}
                    for tok in txt.split()[2:]:
                        k, v = tok.split('=')
                        d[k] = v
                    results.append({
                        'win': int(txt.split()[1]),
                        'snore': float(d['snore']),
                        'top': int(d['top'].split('(')[0]),
                        'tops': float(d['top'].split('(')[1].rstrip(')')),
                        'fe': int(d['fe']), 'inf': int(d['inf'])})
                elif txt.startswith(('DONE', 'ERR')):
                    results.append({'done': txt})
                    return

    t = threading.Thread(target=reader)
    t.start()
    s.sendall(hdr)
    CHUNK = 8192
    for i in range(0, len(pcm), CHUNK):
        s.sendall(pcm[i:i + CHUNK])
    # wait for DONE/ERR
    deadline = time.time() + 600
    while time.time() < deadline:
        if any('done' in r for r in results):
            break
        time.sleep(0.2)
    stop_reader.set()
    try:
        s.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    s.close()
    t.join(timeout=2)
    return results


def float_reference(path):
    """float YAMNet reference snore score per full window."""
    from ai_edge_litert.interpreter import Interpreter
    x = decode_f32(path)
    wins = windows(x)
    if not wins:
        return []
    itp = Interpreter(model_path=FLOAT_MODEL)
    itp.resize_tensor_input(0, [WIN])
    itp.allocate_tensors()
    out = next(d for d in itp.get_output_details() if d['shape'][-1] == 521)
    inp = itp.get_input_details()[0]['index']
    scores = []
    for w in wins:
        itp.set_tensor(inp, w.astype(np.float32))
        itp.invoke()
        scores.append(float(itp.get_tensor(out['index']).reshape(-1)[SNORE_IDX]))
    return scores


def events(scores, th):
    """>=3 consecutive windows above threshold -> count segments (MIN_RUN=3)."""
    n_seg, run = 0, 0
    for v in scores:
        if v >= th:
            run += 1
            if run == 3:
                n_seg += 1
        else:
            run = 0
    return n_seg


def main():
    host, port = None, 9000
    out_path = DEFAULT_OUT
    files = []
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] == '--host':
            host = argv[i + 1]; i += 2
        elif argv[i] == '--port':
            port = int(argv[i + 1]); i += 2
        elif argv[i] == '--out':
            out_path = argv[i + 1]; i += 2
        else:
            files.append(argv[i]); i += 1
    if not host:
        print('usage: mcu_test.py --host <board-ip> [files...]'); return 2
    if not files:
        files = sorted(glob.glob(os.path.join(AUDIO_DIR, '*.mp3')))
    print(f'host={host}:{port}, {len(files)} files')

    rows = []
    for path in files:
        name = os.path.basename(path)
        print(f'>> {name}')
        try:
            mcu = send_file(host, port, path)
        except Exception as e:
            print(f'   send failed: {e}')
            rows.append((name, None, None, None, None, None, None))
            continue
        done = next((r['done'] for r in mcu if 'done' in r), 'DONE? no reply')
        wins = [r for r in mcu if 'win' in r]
        if 'ERR' in done or not wins:
            print(f'   {done}')
            rows.append((name, None, None, None, None, None, None))
            continue
        m = [r['snore'] for r in wins]
        ref = float_reference(path)
        n = min(len(m), len(ref))
        deltas = [abs(m[i] - ref[i]) for i in range(n)]
        agree = sum(1 for i in range(n)
                    if (m[i] >= 0.3) == (ref[i] >= 0.3))
        row = {
            'name': name, 'pos': name.startswith('pos_'),
            'n_win': len(wins), 'mcu_max': max(m) if m else None,
            'ref_max': max(ref) if ref else None,
            'dmax': max(deltas) if deltas else None,
            'dmean': float(np.mean(deltas)) if deltas else None,
            'agree': 100.0 * agree / n if n else None,
            'ev_mcu_03': events(m, 0.3), 'ev_ref_03': events(ref, 0.3),
            'ev_mcu_05': events(m, 0.5), 'ev_ref_05': events(ref, 0.5),
            'inf_avg': int(np.mean([r['inf'] for r in wins])) if wins else None,
            'fe_avg': int(np.mean([r['fe'] for r in wins])) if wins else None,
        }
        rows.append(row)
        print(f"   win={row['n_win']} mcu_max={row['mcu_max']:.3f} "
              f"ref_max={row['ref_max']:.3f} dmean={row['dmean']:.3f} "
              f"agree={row['agree']:.0f}% inf={row['inf_avg']}ms")

    # ---- report ----
    ok = [r for r in rows if isinstance(r, dict) and r['mcu_max'] is not None]
    pos = [r for r in ok if r['pos']]
    neg = [r for r in ok if not r['pos']]
    lines = [
        '# M55 int8 鼾声检测 vs Pi float 参考',
        '',
        f'- 日期: {time.strftime("%Y-%m-%d %H:%M")}  host: {host}:{port}',
        f'- 窗: 15600 样本(0.975s) 步 7800; 事件=连续3窗≥阈值',
        f'- 成功文件: {len(ok)}/{len(rows)} (pos {len(pos)}, neg {len(neg)})',
        '',
        '| 文件 | 窗数 | MCU snore max | float max | 平均|Δ| | 最大|Δ| | 0.3一致率 | 事件@0.3 MCU/ref | 事件@0.5 MCU/ref | 平均推理ms |',
        '|---|---|---|---|---|---|---|---|---|---|',
    ]
    for r in ok:
        lines.append(
            f"| {r['name']} | {r['n_win']} | {r['mcu_max']:.3f} | "
            f"{r['ref_max']:.3f} | {r['dmean']:.3f} | {r['dmax']:.3f} | "
            f"{r['agree']:.0f}% | {r['ev_mcu_03']}/{r['ev_ref_03']} | "
            f"{r['ev_mcu_05']}/{r['ev_ref_05']} | {r['inf_avg']} |")
    for r in rows:
        if not (isinstance(r, dict) and r['mcu_max'] is not None):
            lines.append(f'| {r["name"] if isinstance(r, dict) else r[0]} | FAIL |')
    lines += ['']
    if pos and neg:
        lines += [
            '## 分离度',
            f"- 正样本 MCU snore max 最小: {min(r['mcu_max'] for r in pos):.3f}",
            f"- 负样本 MCU snore max 最大: {max(r['mcu_max'] for r in neg):.3f}",
            f"- 正样本平均最大|Δ|: {np.mean([r['dmax'] for r in pos]):.3f}, "
            f"负样本: {np.mean([r['dmax'] for r in neg]):.3f}",
            f"- 0.3 阈值平均一致率: {np.mean([r['agree'] for r in ok]):.1f}%",
            f"- 推理耗时(平均): {np.mean([r['inf_avg'] for r in ok]):.0f} ms/窗, "
            f"前端 {np.mean([r['fe_avg'] for r in ok]):.0f} ms/窗",
        ]
        lines += ['', '## 验收对照', '']
        pm, nm = min(r['mcu_max'] for r in pos), max(r['mcu_max'] for r in neg)
        ag = np.mean([r['agree'] for r in ok])
        lines.append(f"- 负样本 max ≤ 0.3: {'✅' if nm <= 0.3 else '❌'} ({nm:.3f})")
        lines.append(f"- 正样本 max ≥ 0.6: {'✅' if pm >= 0.6 else '❌'} ({pm:.3f})")
        lines.append(f"- 0.3 一致率 ≥ 90%: {'✅' if ag >= 90 else '❌'} ({ag:.1f}%)")
        lat = np.mean([r['inf_avg'] + r['fe_avg'] for r in ok])
        lines.append(f"- 单窗总耗时 ≤ 500ms: {'✅' if lat <= 500 else '❌'} ({lat:.0f}ms)")
    with open(out_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('report ->', out_path)
    return 0


if __name__ == '__main__':
    sys.exit(main())
