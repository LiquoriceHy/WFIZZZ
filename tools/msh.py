#!/usr/bin/env python3
"""通过串口在设备上执行 msh 命令并回收输出。

用途：无人工干预地启停 snore_server、跑 snore_rt，便于自动化验证。

关键点：这块板子的串口桥（COM27）必须把 DTR 和 RTS 都拉起来才能收到
字节，否则读到的永远是空——这是项目记录里的已知坑。
"""
import argparse
import re
import sys
import time

import serial


PROMPT = re.compile(rb"msh\s*/>")


def drain(ser, quiet_ms=400, max_ms=8000):
    """读到连续 quiet_ms 没有新字节为止，返回解码后的文本。"""
    buf = bytearray()
    deadline = time.time() + max_ms / 1000.0
    last_rx = time.time()

    while time.time() < deadline:
        chunk = ser.read(ser.in_waiting or 1)
        if chunk:
            buf += chunk
            last_rx = time.time()
        else:
            if (time.time() - last_rx) * 1000 > quiet_ms and buf:
                break
            time.sleep(0.02)

    return buf.decode("utf-8", "replace")


def run_cmd(ser, cmd, wait_ms=1500):
    ser.reset_input_buffer()
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    time.sleep(wait_ms / 1000.0)
    return drain(ser)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM27")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("cmds", nargs="+", help="要执行的 msh 命令")
    ap.add_argument("--wait", type=int, default=1500, help="每条命令等待毫秒")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.2)
    except serial.SerialException as e:
        print("open %s failed: %s" % (args.port, e))
        return 1

    # 这块板子的串口桥必须拉起 DTR/RTS 才能收到数据
    ser.dtr = True
    ser.rts = True
    time.sleep(0.3)

    # 唤醒提示符
    ser.write(b"\r\n")
    time.sleep(0.4)
    drain(ser, quiet_ms=300, max_ms=2000)

    for cmd in args.cmds:
        print("$ " + cmd)
        out = run_cmd(ser, cmd, args.wait)
        sys.stdout.write(out)
        if not out.endswith("\n"):
            sys.stdout.write("\n")

    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
