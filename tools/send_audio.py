#!/usr/bin/env python3
"""最小化测试：向 M55 snore_server 发一个音频文件，打印原始回包与耗时。

不复用 mcu_test.py（它还会跑 float 参考模型，采样时长会掩盖真实速度）。
这里只关心：能不能推理、每窗 fe/inf 多少毫秒、总耗时。
"""
import socket
import subprocess
import sys
import time

SR = 16000
WIN = 15600


def decode_i16(path):
    # 已经是裸 s16le PCM 就直接用，避免 ffmpeg 二次转码失败
    if path.endswith('.pcm'):
        return open(path, 'rb').read()
    return subprocess.run(
        ['ffmpeg', '-v', 'error', '-i', path, '-ac', '1', '-ar', str(SR),
         '-f', 's16le', '-'], capture_output=True, check=True).stdout


def main():
    if len(sys.argv) < 4:
        print("usage: send_audio.py <host> <port> <wav> [max_seconds]")
        return 1

    host, port, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    max_s = float(sys.argv[4]) if len(sys.argv) > 4 else 60.0

    pcm = decode_i16(path)
    nsamp = len(pcm) // 2
    dur = nsamp / SR
    nwin = 1 + (nsamp - WIN) // 7800 if nsamp >= WIN else 0

    print("file   : %s" % path)
    print("audio  : %.2f s, %d samples, expect %d windows" % (dur, nsamp, nwin))

    hdr = (b'SNQ1' + bytes([0, 0, 0, 0]) +
           SR.to_bytes(4, 'big') + (16).to_bytes(4, 'big') +
           (1).to_bytes(4, 'big') + nsamp.to_bytes(4, 'big'))

    s = socket.create_connection((host, port), timeout=30)
    s.settimeout(5.0)

    t0 = time.time()
    s.sendall(hdr)
    s.sendall(pcm)

    buf = b''
    lines = []
    deadline = t0 + max_s
    while time.time() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            txt = line.decode('ascii', 'replace').strip()
            lines.append(txt)
            if txt.startswith(('DONE', 'ERR')):
                deadline = 0
                break

    elapsed = time.time() - t0
    s.close()

    print("\n--- reply (%d lines, %.2f s wall) ---" % (len(lines), elapsed))
    for ln in lines[:12]:
        print("  " + ln)
    if len(lines) > 12:
        print("  ... (%d more)" % (len(lines) - 12))

    # 统计
    fe, inf = [], []
    for ln in lines:
        if not ln.startswith("WIN"):
            continue
        for tok in ln.split():
            if tok.startswith("fe="):
                fe.append(int(tok[3:]))
            elif tok.startswith("inf="):
                inf.append(int(tok[4:]))

    if fe:
        print("\n--- timing (per window) ---")
        print("windows   : %d" % len(fe))
        print("fe  ms    : min=%d max=%d avg=%.1f" % (min(fe), max(fe), sum(fe)/len(fe)))
        print("inf ms    : min=%d max=%d avg=%.1f" % (min(inf), max(inf), sum(inf)/len(inf)))
        print("total avg : %.1f ms/window (audio window = 975 ms)" % ((sum(fe)+sum(inf))/len(fe)))
        print("realtime  : %s" % ("YES" if (sum(fe)+sum(inf))/len(fe) <= 975 else "NO"))
    else:
        print("\nno WIN lines -> inference did not produce results")

    return 0


if __name__ == "__main__":
    sys.exit(main())
