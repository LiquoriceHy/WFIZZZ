#!/usr/bin/env python3
"""mel-echo 校验：把一段音频推给 M55（mode 1），M55 回传它算出的 96x64
log-mel patch，与树莓派 numpy 前端逐元素比对。前端一致才能谈推理。

Usage: ~/snore_venv/bin/python mel_check.py --host <board-ip> [--file x.mp3]
"""
import os
import socket
import subprocess
import sys
import threading
import time

import numpy as np

MCU = os.path.dirname(os.path.abspath(__file__))
SR = 16000
WIN = 15600
HOP = 7800
FRAMES, BANDS = 96, 64


def decode_f32(path):
    return np.frombuffer(subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', path, '-ac', '1', '-ar', str(SR),
         '-f', 'f32le', '-'], capture_output=True, check=True).stdout,
        dtype=np.float32)


def numpy_patch(x):
    hann = np.load(os.path.join(MCU, 'const_400.npy')).reshape(-1)
    mel = np.load(os.path.join(MCU, 'const_16448.npy')).astype(np.float32)
    f = 1 + (len(x) - 400) // 160
    framed = np.lib.stride_tricks.as_strided(
        x, (f, 400), (x.strides[0] * 160, x.strides[0]))
    spec = np.abs(np.fft.rfft(framed * hann, n=512, axis=1))
    return np.log(spec @ mel.T + 0.001).astype(np.float32)


def main():
    host, port, path = None, 9000, None
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        if argv[i] == '--host':
            host = argv[i + 1]; i += 2
        elif argv[i] == '--port':
            port = int(argv[i + 1]); i += 2
        elif argv[i] == '--file':
            path = argv[i + 1]; i += 2
        else:
            i += 1
    if not host or not path:
        print('usage: mel_check.py --host <ip> --file <audio>'); return 2

    x = decode_f32(path)
    n_win = 1 + (len(x) - WIN) // HOP if len(x) >= WIN else 0
    if n_win == 0:
        print('file too short'); return 1
    n_win = min(n_win, 3)          # 前 3 窗足够校验
    nsamp = (n_win - 1) * HOP + WIN

    pcm = (np.clip(x[:nsamp], -1.0, 1.0) * 32767.0).astype('<i2').tobytes()
    hdr = b'SNQ1' + bytes([1, 0, 0, 0]) + SR.to_bytes(4, 'big') + \
        (16).to_bytes(4, 'big') + (1).to_bytes(4, 'big') + \
        (nsamp).to_bytes(4, 'big')

    s = socket.create_connection((host, port), timeout=30)
    s.settimeout(None)             # 读取线程改为无限等，超时由主循环控制
    patches, status = [], ['...']
    stop = threading.Event()

    def reader():
        buf = b''
        need = FRAMES * BANDS * 4
        while not stop.is_set():
            try:
                chunk = s.recv(65536)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while len(buf) >= need:
                patches.append(np.frombuffer(buf[:need], dtype='<f4')
                               .reshape(FRAMES, BANDS).copy())
                buf = buf[need:]
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                txt = line.decode('ascii', 'replace').strip()
                if txt.startswith(('DONE', 'ERR')):
                    status[0] = txt
                    return

    t = threading.Thread(target=reader)
    t.start()
    s.sendall(hdr)
    s.sendall(pcm)
    deadline = time.time() + 120
    while time.time() < deadline and status[0] == '...' \
            and len(patches) < n_win:
        time.sleep(0.2)
    stop.set()
    try:
        s.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    s.close()
    t.join(timeout=2)
    print(f'bytes received by reader: {sum(p.size for p in patches) * 4}')

    print('status:', status[0], '| patches received:', len(patches))
    hann = np.load(os.path.join(MCU, 'const_400.npy')).reshape(-1)
    mel = np.load(os.path.join(MCU, 'const_16448.npy')).astype(np.float32)
    ok = True
    for i in range(min(n_win, len(patches))):
        ref = numpy_patch(x[i * HOP:i * HOP + WIN])
        got = patches[i]
        d = np.abs(got - ref)
        print(f'win{i}: max|d|={d.max():.5f} mean|d|={d.mean():.5f} '
              f'ref[{ref.min():.2f},{ref.max():.2f}] '
              f'mcu[{got.min():.2f},{got.max():.2f}]')
        if d.max() > 0.1:
            ok = False
    print('mel check:', 'PASS' if ok and patches else 'FAIL')
    return 0 if ok and patches else 1


if __name__ == '__main__':
    sys.exit(main())
