#!/usr/bin/env python3
"""音频加工台：把一段录音按多种预处理方案各跑一遍，实测哪种能提升识别。

背景：同一段「你好兮兮」录音，人耳听着清晰，但 KWS 与流式 ASR 都几乎识别不出；
而模型官方样本能正常识别。两者的差别可能在采样带宽、频响、电平、语速。
本工具用**同一套模型**对比不同预处理的识别结果，用数据决定该加工成什么样。

用法（工作目录 lvgl_ws/lv_port_pc_vscode）：
  bash scripts/bench.sh ../../.tools/kws/mic_xixi.wav
  bash scripts/bench.sh <wav> --variants raw,tel8k,norm     # 只跑指定方案
  bash scripts/bench.sh <wav> --save-dir D:/temp/variants    # 把加工结果落盘

方案说明：
  raw      原样
  norm     峰值归一到 -3dBFS（只动音量）
  hp120    二阶高通 120Hz（去低频嗡声/桌面震动）
  tel      300–3400Hz 带通（模拟电话/通话耳机频响）
  tel8k    tel 之后再降采样到 8k 再升回 16k（模拟 8k 源，最高 4kHz）
  band8k   仅降采样到 8k 再升回（保留全部低频，砍掉 4k 以上）
  bright   提升 1–4kHz 高频（补偿耳机频响下陷）
  eqmatch  用官方样本的语音长时平均谱做 EQ，把音色拉向官方
  denoise  谱减降噪（估计静音段噪声谱）
  slow95   降速到 0.95 倍（不变调，线性插值）
  fast105  提速到 1.05 倍
"""

import argparse
import os
import sys
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kws_test as K            # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
ASR_DIR = os.path.join(ROOT, ".tools", "asr")
ENC = "encoder-epoch-99-avg-1.int8.onnx"
DEC = "decoder-epoch-99-avg-1.int8.onnx"
JOI = "joiner-epoch-99-avg-1.int8.onnx"
# 官方样本（模型自带的、确认能被识别的音频），用来做音色对齐参考
REF_WAV = os.path.join(ROOT, ".tools", "asr", "test0.wav")


# ---------- 基础 DSP ----------

def fir_lowpass(cut_hz, rate, taps=129):
    n = np.arange(taps) - (taps - 1) / 2.0
    fc = cut_hz / rate
    h = 2 * fc * np.sinc(2 * fc * n)
    h *= np.hamming(taps)
    return h / h.sum()


def fir_highpass(cut_hz, rate, taps=129):
    lp = fir_lowpass(cut_hz, rate, taps)
    hp = -lp
    hp[(taps - 1) // 2] += 1.0
    return hp


def fir_bandpass(lo, hi, rate, taps=129):
    lp_hi = fir_lowpass(hi, rate, taps)
    lp_lo = fir_lowpass(lo, rate, taps)
    bp = lp_hi - lp_lo
    return bp


def apply_fir(x, h):
    return np.convolve(x, h, mode="same")


def to8k_back(x, rate=16000):
    """降到 8k 再线性插值升回 16k，等效把带宽砍到 4kHz。"""
    lp = fir_lowpass(3600, rate)
    y = apply_fir(x, lp)
    y8 = y[::2]
    n_up = np.arange(len(y8) * 2) / 2.0
    return np.interp(n_up, np.arange(len(y8)), y8)


def normalize(x, peak_db=-3.0):
    p = np.abs(x).max()
    if p < 1e-6:
        return x
    target = 32767.0 * (10 ** (peak_db / 20.0))
    return x * (target / p)


def slow(x, rate, factor):
    """不变调改变速度：factor<1 变慢（样本变多）。"""
    n_out = int(len(x) / factor)
    idx = np.linspace(0, len(x) - 1, n_out)
    return np.interp(idx, np.arange(len(x)), x)


# ---------- 谱分析 / 谱域滤波 ----------

def stft(x, n=512, hop=128):
    win = np.hanning(n)
    frames = []
    for i in range(0, max(1, len(x) - n), hop):
        frames.append(x[i:i + n] * win)
    if not frames:
        frames = [np.zeros(n)]
    return np.array(frames), win, hop, n


def istft(F, win, hop, n, length):
    y = np.zeros(length + n)
    wsum = np.zeros(length + n)
    for i, f in enumerate(F):
        seg = np.fft.irfft(f, n) * win
        s = i * hop
        y[s:s + n] += seg
        wsum[s:s + n] += win ** 2
    wsum[wsum < 1e-8] = 1e-8
    y = y / wsum
    return y[:length]


def speech_spectrum(x, rate, n=512, hop=128):
    """返回 (freqs, 扣掉底噪后的语音平均功率谱)。"""
    F, win, hop, n = stft(x, n, hop)
    rms = np.sqrt((F ** 2).mean(axis=1))
    p95 = np.percentile(rms, 95)
    P = np.abs(np.fft.rfft(F, axis=1)) ** 2
    voiced = P[rms > p95 * 0.20]
    sil = P[rms < p95 * 0.03]
    Pv = voiced.mean(axis=0) if len(voiced) else P.mean(axis=0)
    Ps = sil.mean(axis=0) if len(sil) else np.zeros_like(Pv)
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    return freqs, np.maximum(Pv - Ps, 1e-9)


def eq_curve_to_ref(x, rate, ref_wav):
    """按「参考语音谱 / 本段语音谱」逐频点算增益（限幅 ±18dB，平滑）。"""
    xs, _ = K.read_wav_mono16k(ref_wav)
    xs = np.asarray(xs, dtype=np.float64)
    f1, P1 = speech_spectrum(np.asarray(x, dtype=np.float64), rate)
    f2, P2 = speech_spectrum(xs, rate)
    g = np.sqrt(P2 / P1)
    g = np.clip(g, 10 ** (-18 / 20.0), 10 ** (18 / 20.0))
    # 3 点平滑，避免尖锐陷波
    g = np.convolve(g, np.ones(5) / 5.0, mode="same")
    return f1, g


def apply_eq(x, rate, freqs_gain):
    freqs, g = freqs_gain
    F, win, hop, n = stft(np.asarray(x, dtype=np.float64))
    f_fft = np.fft.rfftfreq(n, 1.0 / rate)
    gg = np.interp(f_fft, freqs, g)
    F = np.fft.rfft(F, axis=1) * gg[None, :]
    return istft(F, win, hop, n, len(x))


def spectral_denoise(x, rate, over=1.2, floor=0.05):
    F, win, hop, n = stft(np.asarray(x, dtype=np.float64))
    rms = np.sqrt((F ** 2).mean(axis=1))
    p95 = np.percentile(rms, 95)
    sil = F[rms < p95 * 0.05]
    P = np.abs(np.fft.rfft(F, axis=1)) ** 2
    noise = (np.abs(np.fft.rfft(sil, axis=1)) ** 2).mean(axis=0) if len(sil) else P.min(axis=0)
    mag = np.sqrt(P)
    mag_dn = np.maximum(mag - over * np.sqrt(noise)[None, :], floor * mag)
    phase = np.angle(np.fft.rfft(F, axis=1))
    F2 = mag_dn * np.exp(1j * phase)
    return istft(F2, win, hop, n, len(x))


# ---------- 方案表 ----------

def build_variants(x, rate, save_dir):
    x = np.asarray(x, dtype=np.float64)      # read_wav_mono16k 返回 array.array
    names = ["raw", "norm", "hp120", "tel", "tel8k", "band8k", "bright",
             "eqmatch", "denoise", "slow95", "fast105"]
    out = {"raw": x.copy()}
    out["norm"] = normalize(x)
    out["hp120"] = normalize(apply_fir(x, fir_highpass(120, rate)))
    tel = apply_fir(x, fir_bandpass(300, 3400, rate))
    out["tel"] = normalize(tel)
    out["tel8k"] = normalize(to8k_back(tel, rate))
    out["band8k"] = normalize(to8k_back(x, rate))
    # bright: 提升 1k-4k
    hi = fir_bandpass(1000, 4000, rate)
    out["bright"] = normalize(x + 0.8 * apply_fir(x, hi))
    if os.path.isfile(REF_WAV):
        out["eqmatch"] = normalize(apply_eq(x, rate, eq_curve_to_ref(x, rate, REF_WAV)))
    out["denoise"] = normalize(spectral_denoise(x, rate))
    out["slow95"] = normalize(slow(x, rate, 0.95))
    out["fast105"] = normalize(slow(x, rate, 1.05))
    if save_dir:
        os.makedirs(save_dir, exist_ok=True)
        for k, v in out.items():
            p = os.path.join(save_dir, "mic_%s.wav" % k)
            write_wav(p, v, rate)
            print("  已存 %s" % p)
    return names, out


def write_wav(path, x, rate):
    y = np.clip(x, -32768, 32767).astype(np.int16)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(y.tobytes())


# ---------- 识别 ----------

def build_asr():
    import sherpa_onnx
    return sherpa_onnx.OnlineRecognizer.from_transducer(
        tokens=os.path.join(ASR_DIR, "tokens.txt"),
        encoder=os.path.join(ASR_DIR, ENC),
        decoder=os.path.join(ASR_DIR, DEC),
        joiner=os.path.join(ASR_DIR, JOI),
        num_threads=2, sample_rate=16000, feature_dim=80,
        decoding_method="greedy_search", provider="cpu",
        enable_endpoint_detection=True,
        rule1_min_trailing_silence=2.4,
        rule2_min_trailing_silence=0.8,
        rule3_min_utterance_length=20.0,
    )


def asr_text(rec, pcm, rate):
    s = rec.create_stream()
    for off in range(0, len(pcm), K.CHUNK):
        s.accept_waveform(rate, pcm[off:off + K.CHUNK])
        while rec.is_ready(s):
            rec.decode_stream(s)
    s.input_finished()
    while rec.is_ready(s):
        rec.decode_stream(s)
    return rec.get_result(s) or ""


def kws_hits(kws, pcm, rate):
    s = kws.create_stream()
    n = 0
    for off in range(0, len(pcm), K.CHUNK):
        s.accept_waveform(rate, pcm[off:off + K.CHUNK])
        while kws.is_ready(s):
            kws.decode_stream(s)
        if kws.get_result(s):
            n += 1
            kws.reset_stream(s)
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--kws-dir", default=os.path.join(ROOT, ".tools", "kws"))
    ap.add_argument("--keywords", default="")
    ap.add_argument("--threshold", type=float, default=0.25)
    ap.add_argument("--score", type=float, default=1.0)
    ap.add_argument("--variants", default="")
    ap.add_argument("--save-dir", default="")
    ap.add_argument("--no-asr", action="store_true")
    ap.add_argument("--no-kws", action="store_true")
    a = ap.parse_args()

    kw = a.keywords or os.path.join(a.kws_dir, "keywords_xixi.txt")
    x, rate = K.read_wav_mono16k(a.wav)
    print("源音频 %s  %.2fs  %dHz\n" % (os.path.basename(a.wav),
                                        len(x) / float(rate), rate))
    print("生成加工方案……")
    names, vs = build_variants(x, rate, a.save_dir)
    if a.variants:
        want = [s.strip() for s in a.variants.split(",") if s.strip()]
        names = [n for n in names if n in want]

    rec = None if a.no_asr else build_asr()
    kws = None if a.no_kws else K.load_spotter(a.kws_dir, kw, a.threshold, a.score)

    print("\n%-9s %6s %8s  %s" % ("方案", "KWS", "ASR字数", "ASR 文本（截断 60 字）"))
    print("-" * 78)
    for n in names:
        pcm = vs[n].astype(np.int16)
        kh = "-" if kws is None else "%d 次" % kws_hits(kws, pcm, rate)
        if rec is None:
            print("%-9s %6s %8s  %s" % (n, kh, "-", "-"))
            continue
        t = asr_text(rec, pcm, rate).replace(" ", "")
        has = "★" if ("你好" in t) else " "
        print("%-9s %6s %8d  %s %s" % (n, kh, len(t), has, t[:60]))
    print("\n★ = 文本里出现了「你好」。字数越多通常说明可识别性越好。")


if __name__ == "__main__":
    main()
