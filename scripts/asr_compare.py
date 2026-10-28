#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多引擎离线 ASR 横向对比：同一批音频，同一套唤醒判据，比命中率 / RTF / 体积。

只做三件事，不做花活：
  1. 按目录结构自动认出模型类型并加载（paraformer / zipformer-ctc / dolphin /
     moonshine / qwen3-asr / sense-voice / whisper）；
  2. 对每组测试音频跑识别，原文照抄，不做任何"猜"；
  3. 用两把尺子判命中：严格（文本里真的出现目标词）与近似（近音字兜底）。

用法：
  python scripts/asr_compare.py --models ../../.tools/m_paraformer ../../.tools/asr_sv
  python scripts/asr_compare.py --models ... --cases A,B      # 只跑指定组
"""

import argparse
import json
import os
import sys
import time
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kws_test as K                                       # noqa: E402

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
TOOLS = os.path.join(ROOT, ".tools")


# ---------------------------------------------------------------- 模型类型探测

def detect_kind(d):
    """按目录里的文件名判断是哪种架构，返回 kind 字符串。"""
    names = set(os.listdir(d))
    if {"encoder.int8.onnx", "decoder.int8.onnx", "conv_frontend.onnx"} <= names:
        return "qwen3_asr"
    if "encoder_model.ort" in names or "encoder_model.onnx" in names:
        return "moonshine"
    if "model.int8.onnx" in names or "model.onnx" in names:
        m = "model.int8.onnx" if "model.int8.onnx" in names else "model.onnx"
        # paraformer 的 onnx 元信息里带 paraformer 字段，这里靠目录名兜底
        tag = os.path.basename(d).lower()
        if "paraformer" in tag:
            return "paraformer"
        if "dolphin" in tag:
            return "dolphin"
        if "zipctc" in tag or "zipformer" in tag:
            return "zipformer_ctc"
        if "sense" in tag or "asr_sv" in tag:
            return "sensevoice"
        return "unknown_single:" + m
    return "unknown"


def build(d, threads=4):
    """返回 (recognizer, meta)。meta 里带类型和文件体积。"""
    import sherpa_onnx as so
    kind = detect_kind(d)
    tok = os.path.join(d, "tokens.txt")
    size = 0
    for f in os.listdir(d):
        p = os.path.join(d, f)
        if os.path.isfile(p):
            size += os.path.getsize(p)
    t0 = time.perf_counter()

    if kind == "paraformer":
        f = "model.int8.onnx" if os.path.exists(os.path.join(d, "model.int8.onnx")) else "model.onnx"
        r = so.OfflineRecognizer.from_paraformer(
            paraformer=os.path.join(d, f), tokens=tok,
            num_threads=threads, debug=False, provider="cpu")
    elif kind == "zipformer_ctc":
        f = "model.int8.onnx" if os.path.exists(os.path.join(d, "model.int8.onnx")) else "model.onnx"
        # 该类模型即使带 bbpe.model，sherpa-onnx 2.0.1 的 from_zipformer_ctc 也没有
        # bpe_vocab 参数（词表已烘进 tokens.txt），多传会直接报 unexpected keyword。
        r = so.OfflineRecognizer.from_zipformer_ctc(
            model=os.path.join(d, f), tokens=tok,
            num_threads=threads, debug=False, provider="cpu")
    elif kind == "dolphin":
        f = "model.int8.onnx" if os.path.exists(os.path.join(d, "model.int8.onnx")) else "model.onnx"
        r = so.OfflineRecognizer.from_dolphin_ctc(
            model=os.path.join(d, f), tokens=tok,
            num_threads=threads, debug=False, provider="cpu")
    elif kind == "moonshine":
        # 新导出（encoder_model.ort + decoder_model_merged.ort 两文件）走 v2 接口；
        # 老的三/四文件版才有 preprocessor 与 uncached/cached 之分。
        if os.path.exists(os.path.join(d, "decoder_model_merged.ort")):
            r = so.OfflineRecognizer.from_moonshine_v2(
                encoder=os.path.join(d, "encoder_model.ort"),
                decoder=os.path.join(d, "decoder_model_merged.ort"),
                tokens=tok, num_threads=threads, debug=False, provider="cpu")
        else:
            pre = ""
            for cand in ("preprocess.onnx", "preprocessor.onnx"):
                p2 = os.path.join(d, cand)
                if os.path.exists(p2):
                    pre = p2
                    break
            r = so.OfflineRecognizer.from_moonshine(
                preprocessor=pre,
                encoder=os.path.join(d, "encoder_model.ort"),
                uncached_decoder=os.path.join(d, "decode_model.ort"),
                cached_decoder=os.path.join(d, "cached_decode_model.ort"),
                tokens=tok, num_threads=threads, debug=False, provider="cpu")
    elif kind == "qwen3_asr":
        r = so.OfflineRecognizer.from_qwen3_asr(
            conv_frontend=os.path.join(d, "conv_frontend.onnx"),
            encoder=os.path.join(d, "encoder.int8.onnx"),
            decoder=os.path.join(d, "decoder.int8.onnx"),
            tokenizer=os.path.join(d, "tokenizer"),
            num_threads=threads, debug=False, provider="cpu")
    elif kind == "sensevoice":
        f = "model.int8.onnx" if os.path.exists(os.path.join(d, "model.int8.onnx")) else "model.onnx"
        r = so.OfflineRecognizer.from_sense_voice(
            model=os.path.join(d, f), tokens=tok,
            num_threads=threads, use_itn=True, language="zh",
            debug=False, provider="cpu")
    else:
        raise RuntimeError("认不出模型类型: %s (%s)" % (d, kind))

    return r, {"kind": kind, "bytes": size, "load_s": time.perf_counter() - t0}


# ---------------------------------------------------------------- 唤醒判据

PUNCT = "，。！？、,.!? \u3000:：;；\"'“”‘’（）()"


def clean(t):
    return "".join(c for c in t if c not in PUNCT)


# 严格：文本里必须真的出现这个词形
STRICT = {
    "xiaozhi": ["小智", "晓智"],
    "xixi": ["兮兮"],
}
# 近似：声母韵母都近的字，人都未必分得清，模型更分不清
NEAR = {
    "xiaozhi": set("智志知治制质资姿紫子致至"),
    "xixi": set("兮西嘻熙希溪喜息细析心欣新鑫"),
}


def judge(text, target):
    """返回 (strict_hit, near_hit)。"""
    t = clean(text)
    if not t:
        return False, False
    strict = any(w in t for w in STRICT[target])
    if strict:
        return True, True
    near = False
    i = t.find("你好")
    if i >= 0 and i + 3 <= len(t):
        seg = t[i + 2:i + 4]
        near = any(c in NEAR[target] for c in seg)
    if not near:
        near = any(c in NEAR[target] for c in t[-6:])
    return False, near


# ---------------------------------------------------------------- 测试集

def case_a():
    """手机录音，4 遍独立的「你好小智」（已按 SenseVoice 时间戳切好）。"""
    d = os.path.join(TOOLS, "rec", "xiaozhi_phone")
    files = [os.path.join(d, "p%02d.wav" % i) for i in (1, 2, 3, 4)]
    files = [f for f in files if os.path.exists(f)]
    return "A 手机录音·你好小智（逐段 4 条）", files, "xiaozhi"


def case_b():
    """手机录音整条 5.3s，4 遍连说。测"不切段"的整体识别。"""
    f = os.path.join(TOOLS, "rec", "1028_2.wav")
    return ("B 手机录音·整条连说", [f] if os.path.exists(f) else [], "xiaozhi")


def case_c():
    """Jabra 录音 12 句「你好兮兮」，按能量切句。"""
    f = os.path.join(TOOLS, "kws", "mic_xixi.wav")
    if not os.path.exists(f):
        return "C Jabra·你好兮兮（12 句）", [], "xixi"
    pcm, rate = K.read_wav_mono16k(f)
    segs = K.split_speech(pcm, rate)
    tmp = os.path.join(TOOLS, "rec", "xixi_seg")
    os.makedirs(tmp, exist_ok=True)
    out = []
    for i, (s, e) in enumerate(segs, 1):
        part = pcm[s:e]
        tail = np.zeros(int(rate * 0.5), dtype=np.int16)
        p = os.path.join(tmp, "s%02d.wav" % i)
        with wave.open(p, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(np.concatenate([part, tail]).tobytes())
        out.append(p)
    return "C Jabra·你好兮兮（%d 句）" % len(out), out, "xixi"


CASES = {"A": case_a, "B": case_b, "C": case_c}


# ---------------------------------------------------------------- 主流程

def run_one(rec, files, target, verbose=False, warn_long_s=0):
    """对一组音频跑识别，返回统计。

    warn_long_s > 0 时，超过该秒数的音频会提示——Qwen3-ASR 有 512 token 上限
    （约 38s），超了不会抛异常，而是把音频占位符截掉、最后输出空串，非常难查。
    """
    strict = near = 0
    audio_s = 0.0
    infer_s = 0.0
    rows = []
    for f in files:
        pcm, rate = K.read_wav_mono16k(f)
        x = np.asarray(pcm, dtype=np.float32) / 32768.0
        dur = len(x) / float(rate)
        if warn_long_s and dur > warn_long_s:
            print("      [警告] %s 长度 %.1fs 超过该模型上限 %.0fs，会被截断"
                  % (os.path.basename(f), dur, warn_long_s))
        t0 = time.perf_counter()
        s = rec.create_stream()
        s.accept_waveform(rate, x)
        rec.decode_stream(s)
        dt = time.perf_counter() - t0
        text = s.result.text.strip()
        if warn_long_s and not text:
            print("      [提示] %s 输出为空：该段可能无有效语音，或超长被截断"
                  % os.path.basename(f))
        audio_s += dur
        infer_s += dt
        sh, nh = judge(text, target)
        strict += 1 if sh else 0
        near += 1 if nh else 0
        rows.append((os.path.basename(f), dur, dt, text, sh, nh))
        if verbose:
            print("      %-12s %5.2fs  %5.3fs  %s  %s"
                  % (os.path.basename(f), dur, dt, "★" if nh else " ", text))
    n = max(1, len(files))
    return {
        "n": len(files), "strict": strict, "near": near,
        "audio_s": audio_s, "infer_s": infer_s,
        "rtf": infer_s / audio_s if audio_s else 0.0,
        "rows": rows,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", nargs="+", required=True, help="模型目录列表")
    ap.add_argument("--cases", default="A,B,C")
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--json", help="把结果写到这个 json 文件")
    a = ap.parse_args()

    cases = [CASES[c.strip().upper()]() for c in a.cases.split(",") if c.strip().upper() in CASES]
    cases = [c for c in cases if c[1]]
    if not cases:
        K.die("没有可用的测试音频")

    print("测试集：")
    for name, files, tg in cases:
        print("  %-34s %2d 条" % (name, len(files)))
    print()

    result = []
    for md in a.models:
        md = os.path.abspath(md)
        if not os.path.isdir(md):
            print("跳过（目录不存在）: %s" % md)
            continue
        tag = os.path.basename(md)
        try:
            rec, meta = build(md, a.threads)
        except Exception as e:
            print("加载失败 %-22s %s" % (tag, e))
            continue
        print("── %s  [%s]  %.0fMB  加载 %.2fs"
              % (tag, meta["kind"], meta["bytes"] / 1048576.0, meta["load_s"]))
        item = {"name": tag, "kind": meta["kind"],
                "mb": round(meta["bytes"] / 1048576.0, 1),
                "load_s": round(meta["load_s"], 2), "cases": {}}
        for name, files, tg in cases:
            st = run_one(rec, files, tg, a.verbose,
                         warn_long_s=30 if meta["kind"] == "qwen3_asr" else 0)
            item["cases"][name] = {k: v for k, v in st.items() if k != "rows"}
            item["cases"][name]["texts"] = [r[3] for r in st["rows"]]
            print("   %-34s 严格 %d/%d  近似 %d/%d   RTF %.3f"
                  % (name, st["strict"], st["n"], st["near"], st["n"], st["rtf"]))
        result.append(item)
        print()

    # 汇总表
    if result:
        hdr = "%-22s %7s %8s" % ("模型", "体积MB", "加载s")
        for name, _, _ in cases:
            hdr += " %-14s" % name.split("·")[0].split()[0]
        print(hdr)
        for it in result:
            line = "%-22s %7.0f %8.2f" % (it["name"], it["mb"], it["load_s"])
            for name, _, _ in cases:
                c = it["cases"][name]
                line += " %-14s" % ("%d/%d (近%d)" % (c["strict"], c["n"], c["near"]))
            print(line)

    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(result, f, ensure_ascii=False, indent=2)
        print("\n已写入 %s" % a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
