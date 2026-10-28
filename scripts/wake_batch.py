#!/usr/bin/env python3
"""一条长录音 → 逐词 KWS 检出率对比表。

为什么需要：手机录音一次录很多句最省事，但切句后要知道每句是哪个词。
本工具用两条独立线索定分组：
  1. 顺序（首选）：如果切出的句数正好等于 词数×遍数，直接按顺序分组，最可靠；
  2. 识别文本（兜底）：句数对不上时，用 SenseVoice 识别每句，按文本相似度归类，
     并把归不上的句子单独列出来，避免静默错位。

支持的输入格式：wav / mp3 / m4a / amr / aac / ogg（ffmpeg 统一转 16k 单声道）。

用法（工作目录 lvgl_ws/lv_port_pc_vscode）：
  bash scripts/batch.sh ~/Downloads/唤醒词.m4a
  bash scripts/batch.sh 录音.m4a --words 你好小智,小智小智,小智 --repeat 5
  bash scripts/batch.sh 录音.m4a --show-text        # 额外打印每句识别文本
"""

import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kws_test as K            # noqa: E402
import wake_sv as SV            # noqa: E402
import prosody_match as PM      # noqa: E402
import audio_bench as AB        # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
KWS_DIR = os.path.join(ROOT, ".tools", "kws")

DEFAULT_WORDS = "你好小智,小智小智,小智,你好兮兮"
# 候选词 → 关键词文件
KW_FILE = {
    "你好小智": "keywords_xiaozhi.txt",
    "小智小智": "keywords_zzzz.txt",
    "小智": "keywords_zz.txt",
    "你好兮兮": "keywords_xixi.txt",
    "你好西西": "keywords_xixi.txt",
}


def edit_distance(a, b):
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def classify(text, words):
    """把一句识别文本归到候选词，返回 (词, 距离) 或 (None, 大数)。"""
    t = SV.clean(text)
    if not t:
        return None, 99
    best, bestd = None, 99
    for w in words:
        d = edit_distance(t, w)
        if d < bestd:
            best, bestd = w, d
    # 也允许"文本里包含候选词"的强匹配
    for w in words:
        if w and w in t:
            return w, 0
    return best, bestd


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("audio")
    ap.add_argument("--words", default=DEFAULT_WORDS)
    ap.add_argument("--repeat", type=int, default=5)
    ap.add_argument("--threshold", type=float, default=0.25)
    ap.add_argument("--score", type=float, default=1.0)
    ap.add_argument("--show-text", action="store_true")
    a = ap.parse_args()

    words = [w.strip() for w in a.words.split(",") if w.strip()]
    src = PM.to_wav16k(os.path.abspath(a.audio))
    pcm, rate = K.read_wav_mono16k(src)
    pcm = np.asarray(pcm, dtype=np.int16)
    segs = K.split_speech(pcm, rate)
    expect = len(words) * a.repeat
    print("音频 %s  %.1fs，切出 %d 句（按 %d 词 × %d 遍 应为 %d 句）\n"
          % (os.path.basename(a.audio), len(pcm) / float(rate), len(segs),
             len(words), a.repeat, expect))

    rc = SV.Recognizer()
    texts, groups = [], []
    if len(segs) == expect:
        print("[分组] 句数吻合，按顺序分组（最可靠）")
        for i, (s, e) in enumerate(segs):
            t = rc(np.asarray(pcm[s:e], dtype=np.int16), rate)
            texts.append(t)
            groups.append(words[i // a.repeat])
    else:
        print("[分组] 句数不吻合，改用识别文本归类（请核对 --show-text 输出）")
        for i, (s, e) in enumerate(segs):
            t = rc(np.asarray(pcm[s:e], dtype=np.int16), rate)
            texts.append(t)
            w, d = classify(t, words)
            groups.append(w if d <= max(2, len(w or "") // 2) else None)

    if a.show_text:
        print("\n逐句识别：")
        for i, (t, g) in enumerate(zip(texts, groups), 1):
            print("  %2d  %-16s → %s" % (i, t or "-", g or "（未归类）"))
        print()

    # 逐词统计
    print("%-10s %6s %10s  %s" % ("候选词", "句数", "KWS检出", "关键词"))
    print("-" * 66)
    rows = []
    for w in words:
        kw = KW_FILE.get(w)
        idx = [i for i, g in enumerate(groups) if g == w]
        if not kw or not os.path.isfile(os.path.join(KWS_DIR, kw)):
            print("%-10s %6d %10s  （无关键词文件）" % (w, len(idx), "-"))
            continue
        kws = K.load_spotter(KWS_DIR, os.path.join(KWS_DIR, kw), a.threshold, a.score)
        hit = 0
        for i in idx:
            s, e = segs[i]
            part = np.asarray(pcm[s:e], dtype=np.int16)
            if AB.kws_hits(kws, part, rate):
                hit += 1
        rows.append((w, len(idx), hit, kw))
        print("%-10s %6d %8d/%-2d  %s" % (w, len(idx), hit, len(idx), kw))

    if rows:
        print("\n检出率：")
        for w, n, h, _ in rows:
            bar = "█" * int(round(20.0 * h / max(1, n)))
            print("  %-10s %3.0f%%  %s" % (w, 100.0 * h / max(1, n), bar))
        best = max(rows, key=lambda r: (r[2] / max(1, r[1]), -r[1]))
        if best[2]:
            print("\n最可靠：%s（%d/%d）" % (best[0], best[2], best[1]))
    unclassified = sum(1 for g in groups if g is None)
    if unclassified:
        print("\n注意：有 %d 句未归类，上面的分母偏小，建议加 --show-text 核对。" % unclassified)
    return 0


if __name__ == "__main__":
    sys.exit(main())
