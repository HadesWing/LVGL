#!/usr/bin/env python3
"""节奏/音色对齐台：把用户录音逐句"克隆"成参考音的节奏与音色，再回头测识别。

为什么单独做这一件：之前的 audio_bench 只做了「整体变速 ±5%」和「频谱 EQ」，
没有做**逐帧时间规整**。用户的原话是"优化到跟测试音频一样的节奏、一样的音质"，
严格意义上这需要 DTW 把每一句的时间轴 warp 到参考音的时间轴上，让每个音节的
长短、字间停顿的位置都与参考音一致，而不只是把整句拉长或缩短。

四组对照（同一批句子、同一套模型）：
  raw     原句（基线）
  warp    仅节奏对齐（DTW 时间规整到参考音时长结构）
  eq      仅音色对齐（按参考音长时平均谱做逐频点增益）
  warp+eq 两者叠加 —— 理论上"最像参考音"的版本

用法（工作目录 lvgl_ws/lv_port_pc_vscode）：
  bash scripts/prosody.sh ../../.tools/kws/mic_xixi.wav
  bash scripts/prosody.sh <wav> --ref ../../.tools/ref/标准音_你好兮兮_KWS必检出.mp3
  bash scripts/prosody.sh <wav> --save-dir D:/temp/prosody    # 落盘四组音频
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
import audio_bench as AB        # noqa: E402
import wake_sv as SV            # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
DEFAULT_REF = os.path.join(ROOT, ".tools", "ref", "标准音_你好兮兮_KWS必检出.mp3")

RATE = 16000
NFFT = 512
WIN = 400            # 25ms
HOP = 160            # 10ms
NMELS = 40


# ---------- 特征 ----------

def mel_filterbank(rate=RATE, n_fft=NFFT, n_mels=NMELS, fmin=20.0, fmax=None):
    fmax = fmax or rate / 2.0

    def hz2mel(f):
        return 2595.0 * np.log10(1.0 + f / 700.0)

    def mel2hz(m):
        return 700.0 * (10 ** (m / 2595.0) - 1.0)

    mels = np.linspace(hz2mel(fmin), hz2mel(fmax), n_mels + 2)
    hz = mel2hz(mels)
    bins = np.floor((n_fft + 1) * hz / rate).astype(int)
    fb = np.zeros((n_mels, n_fft // 2 + 1))
    for i in range(n_mels):
        l, c, r = int(bins[i]), int(bins[i + 1]), int(bins[i + 2])
        if c <= l:
            c = l + 1
        if r <= c:
            r = c + 1
        fb[i, l:c] = (np.arange(l, c) - l) / float(c - l)
        fb[i, c:r] = (r - np.arange(c, r)) / float(r - c)
    return fb


FB = mel_filterbank()


def logmel(x, rate=RATE):
    """帧化 → 功率谱 → mel → log → 逐句 CMVN。返回 (T, 40)。"""
    x = np.asarray(x, dtype=np.float64)
    if len(x) < WIN:
        x = np.pad(x, (0, WIN - len(x)))
    T = 1 + (len(x) - WIN) // HOP
    idx = np.arange(WIN)[None, :] + HOP * np.arange(T)[:, None]
    fr = x[idx] * np.hamming(WIN)
    P = np.abs(np.fft.rfft(fr, NFFT)) ** 2
    L = np.log(P @ FB.T + 1e-8)
    return (L - L.mean(0)) / (L.std(0) + 1e-5)


# ---------- DTW ----------

def dtw_map(src, ref):
    """返回 (m, smap, cost)。

    m    长度 len(ref)：参考第 j 帧 ← 源第 m[j] 帧（用于把源压到参考时长）
    smap 长度 len(src)：源第 i 帧 ← 参考第 smap[i] 帧（用于把参考节奏铺回源时长）
    """
    T1, T2 = len(src), len(ref)
    d = np.linalg.norm(src[:, None, :] - ref[None, :, :], axis=2)
    INF = 1e18
    D = np.full((T1 + 1, T2 + 1), INF)
    D[0, 0] = 0.0
    for i in range(1, T1 + 1):
        for j in range(1, T2 + 1):
            D[i, j] = d[i - 1, j - 1] + min(D[i - 1, j - 1], D[i - 1, j], D[i, j - 1])
    path = []
    i, j = T1, T2
    while i > 0 and j > 0:
        path.append((i - 1, j - 1))
        m = min(D[i - 1, j - 1], D[i - 1, j], D[i, j - 1])
        if D[i - 1, j - 1] == m:
            i, j = i - 1, j - 1
        elif D[i - 1, j] == m:
            i -= 1
        else:
            j -= 1
    path.reverse()

    out = np.zeros(T2, dtype=int)          # 参考帧 → 源帧
    last, ptr = 0, 0
    for jj in range(T2):
        while ptr < len(path) and path[ptr][1] < jj:
            ptr += 1
        if ptr < len(path) and path[ptr][1] == jj:
            last = path[ptr][0]
        out[jj] = last

    smap = np.zeros(T1, dtype=float)       # 源帧 → 参考帧
    seen = np.zeros(T1, dtype=bool)
    for pi, pj in path:
        smap[pi] = pj
        seen[pi] = True
    nxt = 0.0
    for ii in range(T1):                   # 对齐步跳过的帧用前值填，保证单调不减
        if seen[ii]:
            nxt = smap[ii]
        smap[ii] = nxt
    cost = float(D[T1, T2] / (T1 + T2))
    return out, smap, cost


def warp_to(x, m):
    """按映射表 m 把 x 重新计时；输出时长 = len(m) 帧。保持段复制样本，不插值以免糊。"""
    n_out = len(m) * HOP + WIN
    pos = np.interp(np.arange(n_out) / float(HOP), np.arange(len(m)), m)
    src = np.clip((pos * HOP).astype(int), 0, len(x) - 1)
    y = np.asarray(x, dtype=np.float64)[src]
    k = 5
    return np.convolve(y, np.ones(k) / k, mode="same")


def shape_map(smap, T1, T2):
    """把参考音的节奏铺回源句总时长，返回「输出帧 → 源帧」数组。

    关键点：必须是正向映射 smap（源→参考）再乘尺度，不能用 ref→src 反查，
    反查只覆盖 m 的值域，句首句尾会整段丢失（第一版就踩了这个坑，
    shape 组掉到 17% 是被截断造成的假象，不是节奏对齐本身的锅）。
    """
    pos = np.asarray(smap, dtype=float) * (T1 / float(T2))
    n_out = int(round(pos[-1])) + 1
    k = np.arange(max(1, n_out))
    i = np.clip(np.searchsorted(pos, k, side="right") - 1, 0, T1 - 1)
    return i


# ---------- 静音裁剪 ----------

def trim(pcm, rate=RATE, rel=0.05, floor=60.0, head_ms=60, tail_ms=120):
    n = int(rate * 0.02)
    e = [float(np.sqrt(np.mean(np.asarray(pcm[i:i + n], dtype=np.float64) ** 2)))
         for i in range(0, max(1, len(pcm) - n), n)]
    if not e:
        return pcm
    thr = max(floor, max(e) * rel)
    on = [i for i, v in enumerate(e) if v > thr]
    if not on:
        return pcm
    s = max(0, on[0] * n - int(rate * head_ms / 1000))
    t = min(len(pcm), (on[-1] + 1) * n + int(rate * tail_ms / 1000))
    return pcm[s:t]


def pad_silence(pcm, rate=RATE, head_ms=200, tail_ms=600):
    h = np.zeros(int(rate * head_ms / 1000), dtype=np.int16)
    t = np.zeros(int(rate * tail_ms / 1000), dtype=np.int16)
    return np.concatenate([h, np.asarray(pcm, dtype=np.int16), t])


# ---------- 参考音 ----------

def to_wav16k(path):
    """任意音频 → 16k 单声道 wav（临时文件）。已是目标格式则原样返回。"""
    if path.lower().endswith(".wav"):
        try:
            with wave.open(path, "rb") as w:
                if w.getnchannels() == 1 and w.getsampwidth() == 2 and w.getframerate() == RATE:
                    return path
        except Exception:
            pass
    ffmpeg, _ = K.find_ffmpeg()
    if not ffmpeg:
        K.die("需要 ffmpeg 转换参考音，但没找到")
    out = os.path.join(tempfile.gettempdir(), "prosody_ref_raw16k.wav")
    cmd = [ffmpeg, "-nostdin", "-y", "-i", path, "-ac", "1", "-ar", str(RATE),
           "-sample_fmt", "s16", out]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode != 0 or not os.path.isfile(out):
        K.die("ffmpeg 转参考音失败: %s" % r.stderr.decode("utf-8", "replace")[-400:])
    return out


def load_ref(path):
    if not os.path.isfile(path):
        K.die("参考音不存在: %s" % path)
    x, rate = K.read_wav_mono16k(to_wav16k(path))
    return trim(np.asarray(x, dtype=np.int16), rate), rate


# ---------- 主流程 ----------

def build_groups(pcm, rate, ref_core, ref_mel, do_eq):
    """每句生成 6 个版本，返回 {组名: [句...]}。

    raw      原句
    fast     纯匀速压缩到参考音时长（只变速度，不改节奏结构）—— 用来隔离"压缩"的影响
    warp     逐帧 DTW 压到参考音时长（节奏 + 总时长都对齐）
    shape    只对齐音节相对比例，总时长保持原样（真正的"节奏一样、语速不变"）
    eq       音色对齐
    shape+eq 节奏结构 + 音色都对齐
    """
    segs = K.split_speech(pcm, rate)
    keys = ["raw", "fast", "warp", "shape", "eq", "shape+eq"]
    groups = {k: [] for k in keys}
    ratios = []
    for s, e in segs:
        part = np.asarray(pcm[s:e], dtype=np.int16)
        core = trim(part, rate)
        mel = logmel(core)
        m, smap, cost = dtw_map(mel, ref_mel)

        w = warp_to(core, m)                                    # 压到参考时长
        sh = warp_to(core, shape_map(smap, len(mel), len(ref_mel)))  # 保时长，只改内部比例
        f = AB.slow(np.asarray(core, dtype=np.float64), rate,
                    len(core) / float(max(1, len(ref_core))))    # 匀速压缩

        ratios.append((len(core) / float(rate), len(w) / float(rate),
                       len(sh) / float(rate), cost))

        groups["raw"].append(pad_silence(core, rate))
        groups["fast"].append(pad_silence(np.clip(f, -32768, 32767).astype(np.int16), rate))
        groups["warp"].append(pad_silence(np.clip(w, -32768, 32767).astype(np.int16), rate))
        groups["shape"].append(pad_silence(np.clip(sh, -32768, 32767).astype(np.int16), rate))
        if do_eq:
            eq = AB.apply_eq(core, rate, AB.eq_curve_to_ref(core, rate, _ref_wav_path))
            seq = AB.apply_eq(sh, rate, AB.eq_curve_to_ref(sh, rate, _ref_wav_path))
            groups["eq"].append(pad_silence(AB.normalize(eq).astype(np.int16), rate))
            groups["shape+eq"].append(pad_silence(AB.normalize(seq).astype(np.int16), rate))
    return groups, ratios, len(segs)


def concat(parts):
    return np.concatenate(parts) if parts else np.zeros(0, dtype=np.int16)


def kws_hits(kws, pcm, rate=RATE):
    return AB.kws_hits(kws, pcm, rate)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--ref", default=DEFAULT_REF)
    ap.add_argument("--kws-dir", default=os.path.join(ROOT, ".tools", "kws"))
    ap.add_argument("--keywords", default="")
    ap.add_argument("--threshold", type=float, default=0.25)
    ap.add_argument("--score", type=float, default=1.0)
    ap.add_argument("--save-dir", default="")
    ap.add_argument("--no-eq", action="store_true")
    a = ap.parse_args()

    global _ref_wav_path
    ref_core, rate = load_ref(a.ref)
    # eq_curve_to_ref 需要磁盘 wav，把参考音落一份到临时目录
    tmp = os.path.join(tempfile.gettempdir(), "prosody_ref16k.wav")
    AB.write_wav(tmp, np.asarray(ref_core, dtype=np.float64), rate)
    _ref_wav_path = tmp
    ref_mel = logmel(ref_core)

    x, rate = K.read_wav_mono16k(a.wav)
    x = np.asarray(x, dtype=np.int16)
    print("源音频 %s  %.2fs" % (os.path.basename(a.wav), len(x) / float(rate)))
    print("参考音 %s  %.2fs（裁静音后）\n" % (os.path.basename(a.ref), len(ref_core) / float(rate)))

    groups, ratios, n = build_groups(x, rate, ref_core, ref_mel, not a.no_eq)
    print("切出 %d 句。时间规整明细：" % n)
    print("     源时长   压到参考  保时长重排   DTW代价")
    for i, (d0, d1, d2, c) in enumerate(ratios, 1):
        print("  %2d  %5.2fs  %5.2fs ×%.2f  %5.2fs ×%.2f   %.2f"
              % (i, d0, d1, d1 / max(d0, 1e-6), d2, d2 / max(d0, 1e-6), c))

    if a.save_dir:
        os.makedirs(a.save_dir, exist_ok=True)
        for k, parts in groups.items():
            if not parts:
                continue
            p = os.path.join(a.save_dir, "prosody_%s.wav" % k.replace("+", "_"))
            AB.write_wav(p, concat(parts).astype(np.float64), rate)
            print("  已存 %s" % p)

    kw = a.keywords or os.path.join(a.kws_dir, "keywords_xixi.txt")
    kws = K.load_spotter(a.kws_dir, kw, a.threshold, a.score)
    rc = SV.Recognizer()

    print("\n%-9s %10s %10s  %s" % ("组", "KWS命中", "SV唤醒", "逐句 SV 文本"))
    print("-" * 96)
    summary = {}
    for k in ["raw", "fast", "warp", "shape", "eq", "shape+eq"]:
        parts = groups.get(k) or []
        if not parts:
            continue
        kh = kws_hits(kws, concat(parts))
        sv_hit, texts = 0, []
        for part in parts:
            t = rc(part, rate)
            if SV.match_wake(t):
                sv_hit += 1
            texts.append(t or "-")
        summary[k] = (kh, sv_hit)
        print("%-9s %7d/%d %8d/%d  %s" % (k, kh, n, sv_hit, n, " | ".join(texts[:4]) + (" ..." if n > 4 else "")))

    print("\n召回率（SV 逐句，%d 句）：" % n)
    for k, (kh, sv) in summary.items():
        print("  %-8s KWS %3.0f%%   SV %3.0f%%" % (k, 100.0 * kh / max(1, n), 100.0 * sv / max(1, n)))
    return 0


_ref_wav_path = ""

if __name__ == "__main__":
    sys.exit(main())
