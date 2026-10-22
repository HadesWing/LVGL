#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""对比"源 wav"与"收到的 wav"，用数字回答"音频通路到底对不对"。

只看"能听见"是骗自己的：字节序反了、采样率错了一倍、SRTP 当明文解出来是噪声，
这几种错误人耳都可能只当"声音有点怪"，而 SNR 会从 ~38dB 掉到个位数。

G.711（A-law/u-law）是 8bit 对数压扩，理论上限 SNR ≈ 38dB（满量程正弦）。
所以判断标准很干脆：
    SNR ≥ 30dB  通路正确，差异就是 G.711 的正常量化噪声
    20~30dB     有明显失真（削顶、增益错、偶发丢帧）
    < 15dB      基本不通（字节序/编码/加密/采样率错）
    ≈ 0dB 或负  两块完全不同的信号

用法：
    python scripts/audio_compare.py test_8k.wav rx.wav
    python scripts/audio_compare.py test_8k.wav rx.wav --seg 1.0
"""
import math
import struct
import sys


def read_wav(path):
    """返回 (samples, rate)。支持 PCM 8/16bit，双声道取左。"""
    with open(path, "rb") as f:
        d = f.read()
    if len(d) < 12 or d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        # 裸 PCM：按 8k mono s16 处理
        n = len(d) // 2
        return list(struct.unpack("<%dh" % n, d[: n * 2])), 8000

    pos, rate, ch, bits, data = 12, 8000, 1, 16, None
    while pos + 8 <= len(d):
        cid = d[pos:pos + 4]
        csz = struct.unpack("<I", d[pos + 4:pos + 8])[0]
        body = d[pos + 8: pos + 8 + csz]
        if cid == b"fmt ":
            _, ch, rate = struct.unpack("<HHI", body[:8])
            bits = struct.unpack("<H", body[14:16])[0]
        elif cid == b"data":
            data = body
            break
        pos += 8 + csz + (csz & 1)

    if data is None:
        return [], rate
    if bits == 16:
        n = len(data) // 2
        allv = struct.unpack("<%dh" % n, data[: n * 2])
    elif bits == 8:
        allv = [(b - 128) * 256 for b in data]     # 8bit wav 是无符号
    else:
        raise SystemExit(f"{path}: 只支持 8/16bit PCM，实际 {bits}bit")

    if ch > 1:
        allv = allv[::ch]                          # 取左声道
    return list(allv), rate


def onset(x, thresh=600, hold=8):
    """第一个"持续超过阈值的样本"位置。用来做粗对齐 —— 比全量互相关快一万倍。"""
    run = 0
    for i, v in enumerate(x):
        if abs(v) > thresh:
            run += 1
            if run >= hold:
                return i - hold + 1
        else:
            run = 0
    return 0


def corr(a, b, off_b, n):
    """sum(a[i]*b[i+off_b])"""
    s = 0
    for i in range(n):
        j = i + off_b
        if j < 0 or j >= len(b):
            break
        s += a[i] * b[j]
    return s


def best_lag(ref, got, lag0, span=64, win=4000):
    """在 lag0 附近细找，返回让相关性最大的偏移（got 相对 ref 的延后量）。"""
    n = min(win, len(ref), len(got) - max(0, lag0))
    if n <= 0:
        return lag0
    best, bestlag = None, lag0
    for d in range(-span, span + 1):
        lag = lag0 + d
        c = corr(ref, got, lag, n)
        if best is None or c > best:
            best, bestlag = c, lag
    return bestlag


def snr_db(ref, got, lag, gain_fit=True):
    """计算 SNR：ref 是参考，got 是实测（按 lag 对齐）。"""
    n = min(len(ref), len(got) - lag)
    if n <= 0:
        return None, None, 0
    se = 0.0
    sg = 0.0
    sr = 0.0
    for i in range(n):
        r = ref[i]
        g = got[i + lag]
        sr += r * r
        sg += g * g
        se += (r - g) * (r - g)
    gain = 1.0
    if gain_fit and sg > 0 and sr > 0:
        num = 0.0
        for i in range(n):
            num += ref[i] * got[i + lag]
        gain = num / sg
        se = 0.0
        for i in range(n):
            d = ref[i] - gain * got[i + lag]
            se += d * d
    if se <= 0:
        return 99.0, gain, n
    if sr <= 0:
        return None, gain, n
    return 10.0 * math.log10(sr / se), gain, n


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    ref_path, got_path = sys.argv[1], sys.argv[2]
    seg = 1.0
    if "--seg" in sys.argv:
        seg = float(sys.argv[sys.argv.index("--seg") + 1])

    ref, rate = read_wav(ref_path)
    got, grate = read_wav(got_path)
    print(f"参考: {ref_path}  {len(ref)} 采样 ({len(ref)/rate:.2f}s, {rate}Hz)")
    print(f"实测: {got_path}  {len(got)} 采样 ({len(got)/grate:.2f}s, {grate}Hz)")
    if not ref or not got:
        raise SystemExit("空文件，检查落盘路径与权限")

    lag = best_lag(ref, got, onset(got) - onset(ref))
    snr, gain, n = snr_db(ref, got, lag)

    print(f"\n对齐: 实测比参考延后 {lag} 采样 = {lag/rate*1000:.1f} ms")
    print(f"增益: {gain:.3f}（1.000 表示电平没被改）")
    if snr is None:
        print("SNR: 无法计算（参考段全静音）")
        return
    verdict = ("通路正确（差异是 G.711 正常量化噪声）" if snr >= 30 else
               "有明显失真，查增益/削顶/丢帧" if snr >= 20 else
               "基本不通，查字节序/编码/加密/采样率" if snr >= 15 else
               "两块信号对不上，可能根本没收到对方音频")
    print(f"SNR: {snr:.1f} dB  →  {verdict}")

    # 分段 SNR：能直接定位"是哪一段坏的"
    step = int(rate * seg)
    print(f"\n分段（每 {seg}s）:")
    for k in range(0, (n + step - 1) // step):
        a = k * step
        b = min(a + step, n)
        s, _g, _ = snr_db(ref[a:b], got, lag + a)
        if s is None:
            print(f"  {a/rate:5.1f}-{b/rate:5.1f}s   （静音段）")
        else:
            mark = "  <<< 这一段有问题" if s < 20 else ""
            print(f"  {a/rate:5.1f}-{b/rate:5.1f}s   {s:6.1f} dB{mark}")


if __name__ == "__main__":
    main()
