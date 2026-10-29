#!/usr/bin/env python3
"""NPU 生效验证：自检 -> 起服务 -> 从树莓派发正负样本 -> 汇总耗时。

用途：一次跑完 NPU 验证全流程，避免反复手工敲串口命令。
前提：COM27 未被其他程序占用；设备已联网（192.168.1.50）。
"""
import argparse
import functools
import re
import subprocess
import sys
import time

sys.path.insert(0, __file__.rsplit('/', 1)[0] if '/' in __file__ else '.')
from msh import drain, run_cmd
import serial

# 后台运行时 stdout 被重定向到文件，默认块缓冲，日志会一直不出现。
print = functools.partial(print, flush=True)


def open_port(port, baud, retries=300, delay=2.0):
    for i in range(retries):
        try:
            ser = serial.Serial(port, baud, timeout=0.2)
            ser.dtr = True
            ser.rts = True
            time.sleep(0.3)
            if i:
                print("  [ok] %s acquired after %ds" % (port, int(i * delay)))
            return ser
        except Exception as e:
            # 每 15 次（约 30s）报一次，避免刷屏但能看出仍在等待
            if i % 15 == 0:
                print("  [wait] %s busy (%s), %ds elapsed -- close the serial terminal in your IDE"
                      % (port, type(e).__name__, int(i * delay)), flush=True)
            time.sleep(delay)
    print("  [fail] %s still busy after %ds" % (port, int(retries * delay)))
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM27")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--host", default="192.168.1.50")
    ap.add_argument("--srv-port", type=int, default=9000)
    ap.add_argument("--pi", default="pi")
    ap.add_argument("--pos", default="/tmp/npu_test_pos.pcm")
    ap.add_argument("--neg", default="/tmp/npu_test_neg.pcm")
    args = ap.parse_args()

    ser = open_port(args.port, args.baud)
    if ser is None:
        print("port never became free; abort")
        return 2

    try:
        ser.write(b"\r\n")
        time.sleep(0.4)
        drain(ser, quiet_ms=300, max_ms=2000)

        print("\n=== [1/4] snore_selftest ===")
        out = run_cmd(ser, "snore_selftest", wait_ms=15000)
        print(out.strip())

        if "NPU ready" not in out:
            print("\n!! NPU not reported ready -- stopping before audio test")
            return 3

        m = re.search(r"fe=(\d+)ms inf=(\d+)ms", out)
        if m:
            print("\nselftest: fe=%s ms  inf=%s ms" % (m.group(1), m.group(2)))

        print("\n=== [2/4] snore_server start ===")
        out = run_cmd(ser, "snore_server start", wait_ms=6000)
        print(out.strip())

        if "listening" not in out:
            print("\n!! server did not start")
            return 4

        print("\n=== [3/4] audio from Pi ===")
        for label, path in (("POS (snore)", args.pos), ("NEG (fan)", args.neg)):
            print("\n--- %s : %s ---" % (label, path))
            cmd = ["ssh", args.pi,
                   "python3 ~/snore_check/send_audio.py %s %d %s 80"
                   % (args.host, args.srv_port, path)]
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
                sys.stdout.write(r.stdout)
                if r.returncode != 0:
                    sys.stdout.write(r.stderr[:500])
            except subprocess.TimeoutExpired:
                print("  !! ssh send timed out (device may still be reading)")

        print("\n=== [4/4] stop ===")
        out = run_cmd(ser, "snore_server stop", wait_ms=2000)
        print(out.strip())

        print("\n=== device log tail ===")
        out = run_cmd(ser, "", wait_ms=1200)
        print(out.strip())
    finally:
        ser.close()

    print("\nDONE")
    return 0


if __name__ == "__main__":
    sys.exit(main())
