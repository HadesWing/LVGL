#!/usr/bin/env python3
"""用 Windows SAPI 合成中文语音到 16k 单声道 wav（给 KWS/ASR 做链路验证用）。

为什么需要它：判断"某个唤醒词模型到底吃不吃"要先排除词表拼写错误。
合成音能被检出只说明链路和音素拼接是对的，**不能证明真人能检出**
（实测 TTS 的「你好兮兮」4/4 全中，同一套模型对真人录音只有 0/12）。

用法（工作目录 lvgl_ws/lv_port_pc_vscode）：
  python scripts/tts_gen.py --list                      # 列出可用音色
  python scripts/tts_gen.py --text 你好小智 --out a.wav
  python scripts/tts_gen.py --text 你好小智 --out a.wav --rate -3 --voice Huihui
  python scripts/tts_gen.py --text 你好小智 --out a.wav --speed 0.85   # 再整体变速
"""

import argparse
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kws_test as K            # noqa: E402

RATE = 16000


def list_voices(sp):
    out = []
    vs = sp.GetVoices()
    for i in range(vs.Count):
        out.append(vs.Item(i).GetDescription())
    return out


def pick_voice(sp, want):
    vs = sp.GetVoices()
    for i in range(vs.Count):
        d = vs.Item(i).GetDescription()
        if want and want.lower() in d.lower():
            return vs.Item(i), d
    for i in range(vs.Count):
        d = vs.Item(i).GetDescription()
        if any(k in d for k in ("Huihui", "Chinese", "中文", "Yaoyao", "Kangkang")):
            return vs.Item(i), d
    return vs.Item(0), vs.Item(0).GetDescription()


def synth(text, out_path, rate=0, voice="", volume=100):
    import win32com.client
    sp = win32com.client.Dispatch("SAPI.SpVoice")
    v, desc = pick_voice(sp, voice)
    sp.Voice = v
    sp.Rate = max(-10, min(10, rate))
    sp.Volume = volume
    stream = win32com.client.Dispatch("SAPI.SpFileStream")
    tmp = os.path.join(tempfile.gettempdir(), "tts_raw.wav")
    stream.Open(tmp, 3)          # 3 = SSFMCreateForWrite
    sp.AudioOutputStream = stream
    sp.Speak(text)
    stream.Close()
    return tmp, desc


def to16k(src, dst):
    ffmpeg, _ = K.find_ffmpeg()
    if not ffmpeg:
        K.die("需要 ffmpeg 做重采样")
    cmd = [ffmpeg, "-nostdin", "-y", "-i", src, "-ac", "1", "-ar", str(RATE),
           "-sample_fmt", "s16", dst]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0:
        K.die("ffmpeg 失败: %s" % r.stderr.decode("utf-8", "replace")[-300:])


def speed_change(src, dst, factor):
    """不变调的整数索引重采样（factor<1 变慢）。只用于粗测语速敏感性。"""
    with wave.open(src, "rb") as w:
        rate = w.getframerate()
        n = w.getnframes()
        x = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64)
    n_out = int(len(x) / factor)
    idx = np.clip(np.linspace(0, len(x) - 1, n_out).astype(int), 0, len(x) - 1)
    y = np.convolve(x[idx], np.ones(5) / 5.0, mode="same")
    write16(dst, y, rate)


def write16(path, x, rate=RATE):
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.clip(x, -32768, 32767).astype(np.int16).tobytes())


def read16(path):
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        n = w.getnframes()
        x = np.frombuffer(w.readframes(n), dtype=np.int16).astype(np.float64)
    return x, rate


def pitch_shift(x, ratio):
    """resample-then-resample：ratio<1 降调（模拟男声），时长保持不变。"""
    n1 = max(1, int(len(x) / ratio))
    y = np.interp(np.linspace(0, len(x) - 1, n1), np.arange(len(x)), x)
    return np.interp(np.linspace(0, len(y) - 1, len(x)), np.arange(len(y)), y)


def add_noise(x, snr_db, seed=0):
    """按指定 SNR 叠白噪，模拟远场/环境噪声。"""
    rng = np.random.RandomState(seed)
    p = float(np.mean(x ** 2)) or 1.0
    n = rng.standard_normal(len(x)) * np.sqrt(p / (10.0 ** (snr_db / 10.0)))
    y = x + n
    peak = np.abs(y).max()
    if peak > 32000:
        y *= 32000.0 / peak
    return y


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--text", default="你好小智")
    ap.add_argument("--out", default="")
    ap.add_argument("--rate", type=int, default=0, help="SAPI 语速 -10..10")
    ap.add_argument("--voice", default="")
    ap.add_argument("--speed", type=float, default=1.0, help="整体变速，<1 变慢")
    ap.add_argument("--pitch", type=float, default=1.0, help="变调系数，<1 降调（模拟男声）")
    ap.add_argument("--snr", type=float, default=0.0, help="叠加白噪的 SNR(dB)，0 表示不加")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()

    import win32com.client
    sp = win32com.client.Dispatch("SAPI.SpVoice")
    if a.list:
        for i, d in enumerate(list_voices(sp)):
            print("%2d  %s" % (i, d))
        return 0

    if not a.out:
        K.die("需要 --out 指定输出 wav 路径")
    post = a.speed != 1.0 or a.pitch != 1.0 or a.snr > 0
    tmp, desc = synth(a.text, a.out, a.rate, a.voice)
    stage = os.path.join(tempfile.gettempdir(), "tts_16k.wav") if post else a.out
    to16k(tmp, stage)
    if post:
        x, rate = read16(stage)
        if a.pitch != 1.0:
            x = pitch_shift(x, a.pitch)
        if a.speed != 1.0:
            n_out = max(1, int(len(x) / a.speed))
            idx = np.clip(np.linspace(0, len(x) - 1, n_out).astype(int), 0, len(x) - 1)
            x = np.convolve(x[idx], np.ones(5) / 5.0, mode="same")
        if a.snr > 0:
            x = add_noise(x, a.snr)
        write16(a.out, x, rate)
    with wave.open(a.out, "rb") as w:
        dur = w.getnframes() / float(w.getframerate())
    print("已生成 %s  %.2fs  音色=%s  语速=%d  变速=%.2f  变调=%.2f  SNR=%s"
          % (a.out, dur, desc, a.rate, a.speed, a.pitch,
             ("%.0fdB" % a.snr) if a.snr > 0 else "无"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
