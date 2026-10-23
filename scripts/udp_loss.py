#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
RTP 丢包注入代理（故障注入器）

作用：坐在 ffmpeg 和室内机中间，把每个 UDP 包原样转发，**按规则故意丢掉一些**。

为什么需要它，而不是"等真实网络丢包"：
   丢包恢复这种逻辑，靠"等网络抽风"是验不了的 —— 你不知道它什么时候丢、
   丢在哪个位置、丢了之后多久才恢复。可控的故障注入才能给出可复现的判据：
   在第 N 个包上丢 8 个，然后去量"多久之后画面恢复干净"。

怎么用：
   python scripts/udp_loss.py --listen 127.0.0.1:6066 --to 127.0.0.1:6064 \
          --drop 60:8 --drop 200:10

   --listen  我们占的端口（ffmpeg 推这个）
   --to      真正接收端（室内机的视频 RTP 端口）
   --drop    START:COUNT —— 从第 START 个包开始，连续丢 COUNT 个（可重复给）

启动后立刻打印一行 "ready"，脚本靠它判断代理已经起来了。
退出时（SIGTERM/SIGINT）打印统计，也可以写文件（--stats）。
"""

import argparse
import signal
import socket
import sys
import time

g_rx = 0
g_fwd = 0
g_dropped = 0
g_stop = False


def on_sig(signum, frame):
    global g_stop
    g_stop = True


def parse_addr(s, default_host="127.0.0.1"):
    if ":" in s:
        h, p = s.rsplit(":", 1)
        return (h or default_host, int(p))
    return (default_host, int(s))


def main():
    # 计数器是模块级的（退出时要打印），所以必须先声明 global ——
    # 少了这一行，下面 `g_rx += 1` 会被当成"给局部变量赋值"，
    # 读的时候还没绑定，直接 UnboundLocalError（第一版就是这么挂的：
    # 代理起来了、`ready` 也打了，但一收包就崩，表现是"推流端一切正常，接收端什么也没有"）。
    global g_rx, g_fwd, g_dropped

    ap = argparse.ArgumentParser(description="RTP 丢包注入代理")
    ap.add_argument("--listen", required=True, help="监听地址 host:port")
    ap.add_argument("--to", required=True, help="转发目标 host:port")
    ap.add_argument("--drop", action="append", default=[],
                    help="丢包规则 START:COUNT，可重复")
    ap.add_argument("--stats", default="", help="统计输出文件（可选）")
    ap.add_argument("--timeout", type=float, default=0.0,
                    help="空闲多少秒后自动退出（0=等信号）")
    args = ap.parse_args()

    signal.signal(signal.SIGTERM, on_sig)
    signal.signal(signal.SIGINT, on_sig)

    rules = []
    for r in args.drop:
        try:
            a, b = r.split(":")
            rules.append((int(a), int(b)))
        except ValueError:
            sys.exit(f"--drop 格式应为 START:COUNT，收到的是 {r!r}")

    lh, lp = parse_addr(args.listen)
    th, tp = parse_addr(args.to)

    srv = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((lh, lp))
    srv.settimeout(0.2)

    sys.stdout.write("ready\n")
    sys.stdout.flush()

    last = time.time()
    while not g_stop:
        try:
            data, src = srv.recvfrom(2048)
        except socket.timeout:
            if args.timeout and (time.time() - last) > args.timeout:
                break
            continue
        except OSError:
            break
        if not data:
            continue
        last = time.time()
        g_rx += 1

        if any(start <= g_rx < start + count for start, count in rules):
            g_dropped += 1
            continue

        try:
            srv.sendto(data, (th, tp))
        except OSError:
            pass
        g_fwd += 1

    srv.close()
    sys.stdout.write("received=%d forwarded=%d dropped=%d\n" % (g_rx, g_fwd, g_dropped))
    sys.stdout.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
