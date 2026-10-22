#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 8kHz / 单声道 / 16bit 的测试音源（PC 侧工具，只用标准库）。

为什么不用手机录一段人声：
  1) 人声每次都不一样，出问题时没法判断"这次和上次一样吗"；
  2) 编码前后无法逐样本比对，只能靠耳朵，而耳朵对"轻微噪声/偶发丢帧"很不敏感；
  3) 这段是**确定性**的，任何时候重跑结果都可复现。

信号设计（每一段都对应一个可观察的故障）：
  0.0-1.0s  440Hz          —— 低频通路是否通
  1.0-2.0s  静音            —— 静音段有没有被注入噪声/回声（听"沙沙"声）
  2.0-3.0s  1kHz            —— 通话频段中心
  3.0-4.0s  静音
  4.0-5.0s  2kHz            —— 高频是否被削（采样率错/低通过头）
  5.0-6.0s  静音
  6.0-8.0s  1kHz 通断 250ms —— 断续感；丢包在这里最刺耳
  8.0-10s   1kHz 音量阶梯（-6/-12/-18/-24dB）—— 动态范围与 G.711 量化台阶

用法：
    python scripts/gen_test_wav.py                  # → test_8k.wav
    python scripts/gen_test_wav.py out.wav 12       # 自定义文件名/时长
"""
import math
import struct
import sys

RATE = 8000


def silence(sec):
    return [0] * int(RATE * sec)


def tone(freq, sec, amp=12000.0):
    n = int(RATE * sec)
    return [int(amp * math.sin(2 * math.pi * freq * i / RATE)) for i in range(n)]


def gated(freq, sec, on=0.25, off=0.25, amp=12000.0):
    out = []
    t = 0.0
    while len(out) < int(RATE * sec):
        out += tone(freq, on, amp)
        out += silence(off)
        t += on + off
    return out[: int(RATE * sec)]


def steps(freq, sec, amp=12000.0):
    """音量阶梯：每段 -6dB，用来听 G.711 的量化噪声怎么随电平变化"""
    out = []
    seg = sec / 4.0
    for k in range(4):
        out += tone(freq, seg, amp / (2 ** k))
    return out


def taper(samples, ms=10):
    """首尾各做 10ms 淡入淡出。

    这个细节不是"为了好听"，是为了**别把测试信号自己的毛病算到链路上**：
    音源是要循环使用的，如果结尾停在大幅值上、开头又从 0 开始，每 10 秒就会
    出现一个人为的"咔哒" —— 而真实链路的丢包也是"咔哒"。不处理这一点，
    你会花半天去查一个根本不存在的问题。
    """
    n = int(RATE * ms / 1000)
    for i in range(min(n, len(samples))):
        g = i / n
        samples[i] = int(samples[i] * g)
        samples[-1 - i] = int(samples[-1 - i] * g)
    return samples


def write_wav(path, samples):
    data = b"".join(struct.pack("<h", max(-32768, min(32767, s))) for s in samples)
    hdr = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    hdr += b"data" + struct.pack("<I", len(data))
    with open(path, "wb") as f:
        f.write(hdr + data)
    return len(data)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "test_8k.wav"
    s = []
    s += tone(440, 1.0)
    s += silence(1.0)
    s += tone(1000, 1.0)
    s += silence(1.0)
    s += tone(2000, 1.0)
    s += silence(1.0)
    s += gated(1000, 2.0)
    s += steps(1000, 2.0)
    taper(s)

    n = write_wav(path, s)
    print(f"{path}: {len(s)} 采样 = {len(s)/RATE:.1f} 秒, {n} 字节 PCM")
    print("建议：把它当音源循环送，10 秒一轮，连听三轮就能听出所有问题。")


if __name__ == "__main__":
    main()
