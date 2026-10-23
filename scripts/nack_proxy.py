#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
NACK 重传代理 —— 扮演一台"支持 RTX 的门口机"。

背景：
  我们（室内机）的 NACK 是接收方向的能力：收到序号断档就发 RTCP Generic NACK
  （RFC 4585 §6.2.1，PT=205/FMT=1）请对端重传。但测试码流是 ffmpeg 推的，
  ffmpeg 不响应 NACK —— 没有对端配合，NACK 的"补洞"效果永远演示不出来。

做法：
  本代理串在 ffmpeg 和室内机之间：
      ffmpeg ──RTP──> 代理(按 --drop 丢包) ──RTP──> 室内机
      室内机 ──RTCP NACK（RTP+1 口）──> 代理：从缓存里把缺的包重发回去
  代理 = 一个支持 NACK 的发送端的**最小替身**：缓存 + 按请求重发，
  不碰 SDP、不碰编解码。重传走的和正常 RTP 同一个 socket（对端是对称
  RTP 校正过的源地址，室内机把 NACK 发回给谁，我们就从谁那儿收到过包）。

用法：
  python nack_proxy.py --listen 127.0.0.1:16064 --to 127.0.0.1:16262 \
         --drop 60:6 --drop 690:8 --timeout 3

  --drop start:count   按包序号注入丢包（与 udp_loss.py 同语法，可多次）
  --timeout 秒         收不到包 N 秒后退出并打印汇总

判定 NACK 有没有生效，看汇总三个数：
  received = 转发的包   dropped = 注入丢弃的包   resent = 按 NACK 重发的包
  室内机侧对应的证据是 ro stats 里 filled == dropped、gap == 0。
"""

import argparse
import select
import socket
import sys
import time


def parse_addr(s):
    host, _, port = s.rpartition(":")
    if not host or not port:
        sys.exit(f"--listen/--to 格式应为 host:port，收到 {s!r}")
    return host, int(port)


def parse_ranges(specs):
    """'60:6' → 收到的第 60~65 个包（闭区间）。

    语义必须和 udp_loss.py 完全一致：按 **接收顺序** 计数，不是 RTP 序号。
    第一版按 RTP seq 解释，结果 ffmpeg 每次 run 的初始 seq 是随机的，
    丢包位置飘忽不定（同一段规则一轮丢 14 包、一轮丢 8 包）——
    位置不确定，"丢在 GOP 中段"这种精心设计的注入就失去了意义。
    """
    out = []
    for s in specs:
        try:
            a, b = s.split(":")
            start, cnt = int(a), int(b)
        except ValueError:
            sys.exit(f"--drop 格式应为 start:count，收到 {s!r}")
        out.append((start, start + cnt))       # [start, start+cnt)
    return out


def seq_dropped(ranges, rx_no):
    for lo, hi in ranges:
        if lo <= rx_no < hi:
            return True
    return False


def parse_nack(data):
    """解析 Generic NACK，返回缺失序号列表（含 BLP 展开的后续洞）"""
    if len(data) < 16:
        return []
    v = data[0] >> 6
    pt = data[1]
    if v != 2 or pt != 205 or (data[0] & 0x1F) != 1:
        return []
    fcis = []
    # length 字段 = 32bit 字数 - 1；FCI 从第 12 字节起
    words = ((data[2] << 8) | data[3]) + 1
    off = 12
    end = min(words * 4, len(data))
    while off + 4 <= end:
        pid = (data[off] << 8) | data[off + 1]
        blp = (data[off + 2] << 8) | data[off + 3]
        fcis.append((pid, blp))
        off += 4
    seqs = []
    for pid, blp in fcis:
        seqs.append(pid)
        for i in range(16):
            if blp & (1 << i):
                seqs.append((pid + 1 + i) & 0xFFFF)
    return seqs


def main():
    ap = argparse.ArgumentParser(description="NACK 重传代理（最小发送端替身）")
    ap.add_argument("--listen", required=True, help="收 RTP 的地址（ffmpeg 推到这里）")
    ap.add_argument("--to", required=True, help="室内机 RTP 地址")
    ap.add_argument("--drop", action="append", default=[], help="start:count 按序号丢")
    ap.add_argument("--timeout", type=float, default=3.0, help="收不到包 N 秒后退出")
    ap.add_argument("--cache", type=int, default=8192, help="重传缓存的最大包数")
    args = ap.parse_args()

    lhost, lport = parse_addr(args.listen)
    thost, tport = parse_addr(args.to)
    ranges = parse_ranges(args.drop)

    rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtp.bind((lhost, lport))
    # RTCP 惯例 = RTP + 1（RFC 3550 §11）：室内机的 NACK 会打到这来
    rtcp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rtcp.bind((lhost, lport + 1))

    cache = {}          # seq -> packet bytes（供 NACK 重发）
    cache_order = []    # 简单 FIFO 淘汰，防内存无界
    n_rx = n_recv = n_drop = n_resent = n_nack = 0

    rtp.settimeout(0.2)
    last_rx = time.time()
    print(f"ready: rtp {lhost}:{lport} → {thost}:{tport}, rtcp :{lport + 1}"
          f", drop={ranges or '无'}", flush=True)

    try:
        while True:
            # 收包 + NACK 都要响应，select 两个 socket
            r, _, _ = select.select([rtp, rtcp], [], [], 0.2)

            if rtcp in r:
                data, _ = rtcp.recvfrom(2048)
                seqs = parse_nack(data)
                if seqs:
                    n_nack += 1
                    for s in seqs:
                        pkt = cache.get(s)
                        if pkt is not None:
                            rtp.sendto(pkt, (thost, tport))
                            n_resent += 1

            if rtp not in r:
                if time.time() - last_rx > args.timeout:
                    break
                continue

            data, _ = rtp.recvfrom(4096)
            last_rx = time.time()
            if len(data) < 12:
                continue
            seq = (data[2] << 8) | data[3]
            n_rx += 1

            # 先缓存再决定丢不丢 —— 真实发送端是"包已发出网络 + 重传缓冲里留着"，
            # 被丢的（我们注入丢的）包必须也能被 NACK 捞回来。
            # 第一版把"丢弃"放在缓存之前，NACK 来查缓存必然落空（resent=0 的假闭环）。
            cache[seq] = data
            cache_order.append(seq)
            while len(cache_order) > args.cache:
                old = cache_order.pop(0)
                cache.pop(old, None)

            # 丢包按"接收顺序"判 —— 语义与 udp_loss.py 一致（见 parse_ranges）
            if seq_dropped(ranges, n_rx):
                n_drop += 1
                continue        # 不转发 —— 模拟网络里真丢了，等 NACK 再发

            rtp.sendto(data, (thost, tport))
            n_recv += 1
    except KeyboardInterrupt:
        pass

    print(f"done: received={n_rx} dropped={n_drop} nacks={n_nack} "
          f"resent={n_resent}", flush=True)


if __name__ == "__main__":
    main()
