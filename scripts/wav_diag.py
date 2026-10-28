#!/usr/bin/env python3
"""录音质量体检：判断一段 wav 到底适不适合做唤醒/识别。

为什么需要它：KWS/ASR 效果差时，90% 的时间被浪费在调参数上，其实是录音本身不行。
这个脚本给出几个硬指标，一次就能分清「参数问题」「模型问题」「音频问题」。

用法（工作目录 lvgl_ws/lv_port_pc_vscode）：
  bash scripts/diag.sh ../../.tools/kws/mic_xixi.wav
  bash scripts/diag.sh a.wav b.wav            # 多个文件对比

指标与判据：
  DC        直流偏置，|DC| 大 → 采集有偏置，rms 虚高
  SNR       帧能量 p90/p10，>20dB 好，<10dB 基本没救
  ZCR       过零率，语音 0.01~0.25
  平坦度    谱几何均值/算术均值，语音 <0.3（谐波强），宽带噪声 >0.5
  低频占比  80-200Hz 能量占比，被高通滤波时接近 0（通话耳机常见）
"""

import array
import math
import sys
import wave

try:
    import numpy as np
except ImportError:
    np = None

BANDS = [(100, 300), (300, 1000), (1000, 3000), (3000, 6000), (6000, 7800)]


def load(path):
    with wave.open(path, "rb") as w:
        n, ch, sw, rate = (w.getnframes(), w.getnchannels(),
                           w.getsampwidth(), w.getframerate())
        raw = w.readframes(n)
    if sw != 2:
        sys.exit("%s: 只支持 16bit" % path)
    if np is not None:
        a = np.frombuffer(raw, dtype=np.int16).astype(np.float64)
        if ch > 1:
            a = a[::ch]
        return a, rate
    a = array.array("h")
    a.frombytes(raw)
    if ch > 1:
        a = a[::ch]
    return a, rate


def time_stats(a, rate):
    n = len(a)
    if np is not None:
        mean = float(a.mean())
        d = a - mean
        rms = float(np.sqrt((d ** 2).mean()))
        zc = int(np.sum(np.signbit(d[1:]) != np.signbit(d[:-1])))
        win = 800
        m = (n // win) * win
        frames = d[:m].reshape(-1, win)
        energies = np.sqrt((frames ** 2).mean(axis=1))
        energies.sort()
        q = lambda p: float(energies[min(len(energies) - 1, int(p * len(energies)))])  # noqa: E731
        p10, p50, p90, p99 = q(0.10), q(0.50), q(0.90), q(0.99)
    else:
        mean = sum(a) / float(n)
        var = 0.0
        zc = 0
        prev = 0.0
        for v in a:
            d = v - mean
            var += d * d
            if (d >= 0) != (prev >= 0):
                zc += 1
            prev = d
        rms = math.sqrt(var / n)
        energies = []
        for i in range(0, max(1, n - 800 + 1), 800):
            s = 0.0
            for v in a[i:i + 800]:
                d = v - mean
                s += d * d
            energies.append(math.sqrt(s / 800.0))
        energies.sort()
        p10, p50, p90, p99 = (energies[int(p * (len(energies) - 1))]
                              for p in (0.10, 0.50, 0.90, 0.99))
    snr = 20 * math.log10(p90 / p10) if p10 > 0 else 99.0
    return dict(mean=mean, rms=rms, zcr=zc / float(n), p10=p10, p50=p50,
                p90=p90, p99=p99, snr=snr)


def spectral_stats(a, rate):
    if np is None:
        return None
    win, hop = 512, 256
    frames, energies = [], []
    for i in range(0, max(1, len(a) - win), hop):
        f = a[i:i + win]
        if len(f) < win:
            break
        f = f - f.mean()
        frames.append(f)
        energies.append(float(np.sqrt((f ** 2).mean())))
    if not frames:
        return None
    energies = np.array(energies)
    frames = np.array(frames)
    idx = np.argsort(energies)[-max(1, len(energies) // 5):]   # 能量前 20% 的帧
    spec = np.abs(np.fft.rfft(frames[idx] * np.hanning(win), axis=1)).mean(axis=0)
    freqs = np.fft.rfftfreq(win, 1.0 / rate)
    power = spec ** 2
    total = float(power.sum()) or 1e-12
    bands = {}
    for lo, hi in BANDS:
        m = (freqs >= lo) & (freqs < hi)
        bands[(lo, hi)] = 100.0 * float(power[m].sum()) / total
    m = (freqs >= 100) & (freqs < 7800) & (spec > 0)
    s = spec[m]
    flatness = float(np.exp(np.log(s).mean()) / s.mean()) if len(s) else 0.0
    m2 = (freqs >= 80) & (freqs < 200)
    low_pct = 100.0 * float(power[m2].sum()) / total
    return dict(bands=bands, flatness=flatness, low_pct=low_pct)


def report(path):
    a, rate = load(path)
    name = path.replace("\\", "/").split("/")[-1]
    if len(a) == 0:
        print("%s: 空文件" % name)
        return
    t = time_stats(a, rate)
    print("=" * 72)
    print("%s   时长 %.2fs   采样率 %d" % (name, len(a) / float(rate), rate))
    print("  DC=%7.1f   rms=%6.0f   ZCR=%.3f   帧能量 p10=%5.0f p50=%5.0f "
          "p90=%5.0f  SNR≈%4.1fdB"
          % (t["mean"], t["rms"], t["zcr"], t["p10"], t["p50"], t["p90"], t["snr"]))

    s = spectral_stats(a, rate)
    if s:
        print("  带宽: " + "  ".join("%d-%dHz %4.1f%%" % (lo, hi, pct)
                                    for (lo, hi), pct in s["bands"].items()))
        print("  谱平坦度 %.3f（语音<0.3 / 噪声>0.5）   80-200Hz 占比 %.1f%%"
              "（被高通滤波时接近0）" % (s["flatness"], s["low_pct"]))

    verdict = []
    if abs(t["mean"]) > 200:
        verdict.append("直流偏置明显")
    if t["snr"] < 10:
        verdict.append("信噪比过低")
    elif t["snr"] < 18:
        verdict.append("信噪比偏低")
    if not 0.01 <= t["zcr"] <= 0.25:
        verdict.append("过零率异常")
    if s:
        if s["flatness"] > 0.45:
            verdict.append("谱太扁平，以噪声为主（语音谐波几乎看不到）")
        elif s["flatness"] > 0.32:
            verdict.append("谱偏扁平（%.2f），噪声成分偏重，识别会明显掉" % s["flatness"])
        if s["low_pct"] < 5.0:
            verdict.append("80-200Hz 能量仅 %.1f%%，像是被通话耳机的语音频带滤波削掉了"
                           % s["low_pct"])
        elif s["low_pct"] < 40.0:
            verdict.append("80-200Hz 仅 %.1f%%（正常语音的基频区应该更重）"
                           % s["low_pct"])
    print("  判读: " + ("；".join(verdict) if verdict else "指标正常，适合识别"))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        report(p)
    print("=" * 72)
    if np is None:
        print("提示：装了 numpy 才有频谱指标（当前用纯 Python，只输出时域指标）")
