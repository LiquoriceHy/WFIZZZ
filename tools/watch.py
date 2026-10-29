#!/usr/bin/env python3
"""串口实时监视：持续打印设备输出，直到超时或收到指定模式。

用途：一边从树莓派发音频，一边看设备侧日志（推理耗时、错误）。
不发送任何命令，纯被动监听。
"""
import argparse
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM27")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--grep", default=None, help="只打印含该关键字的行")
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    # 这块板子的串口桥必须拉起 DTR/RTS 才能收到数据
    ser.dtr = True
    ser.rts = True
    time.sleep(0.2)

    deadline = time.time() + args.seconds
    buf = b""

    try:
        while time.time() < deadline:
            chunk = ser.read(ser.in_waiting or 1)
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                txt = line.decode("utf-8", "replace").rstrip()
                if not txt:
                    continue
                if args.grep and args.grep not in txt:
                    continue
                print(txt, flush=True)
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()

    return 0


if __name__ == "__main__":
    sys.exit(main())
