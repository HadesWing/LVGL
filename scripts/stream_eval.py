#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""流式 + 端点检测评测（真实上板形态）

与 eval.py 的区别：
  eval.py      = 每条 wav 已经切好、只含一个唤醒词 -> 离线整句识别
                 这个前提在真实场景不存在，所以历史数字虚高。
  stream_eval  = 合成一条连续音流（无关语音 + 唤醒词 + 各种停顿 + 底噪），
                 按 100ms 分块喂进去，靠端点检测自己找句子边界。

指标：
  检出率   唤醒词被端点正确切出且判定命中的比例
  误唤醒   无关语音被切出且判定为唤醒的比例（危险项）
  端点数   实际触发端点数 vs 应有句数（<1 表示多句被合并吃掉）
  延迟     该句音频结束后，还要等多久才触发端点（= 尾随静音阈值，直接决定响应手感）
"""
import os
import sys
import json
import time
import wave
import array
import random

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(encoding="utf-8")
    except Exception:
        pass

import sherpa_onnx

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = os.path.join(ROOT, ".tools")
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import eval as E  # 复用 read_wav / wake_hit / peak_rss_mb / size_mb

SR = 16000
CHUNK_MS = 100
CHUNK = SR * CHUNK_MS // 1000

XI = [os.path.join(T, "rec", "xixi_seg", f"s{i:02d}.wav") for i in range(1, 13)]
XZ = [os.path.join(T, "rec", "xiaozhi_phone", f"p{i:02d}.wav") for i in range(1, 5)]
NOISE_SRC = [os.path.join(T, "cer_set", f"{i:02d}.wav") for i in range(1, 21)]

WORDS = {
    "兮兮": (XI, ["你好兮兮", "兮兮"]),
    "小智": (XZ, ["你好小智", "小智"]),
}


def read_pcm(path):
    s, sr = E.read_wav(path)
    if sr != SR:
        raise ValueError(f"{path} 采样率 {sr} != {SR}，需先重采样")
    return s


def silence(sec, rms=0.0, seed=7):
    """静音段。rms>0 时填白噪声（模拟麦克风本底，纯零会让端点检测过于容易）"""
    n = int(SR * sec)
    if rms <= 0:
        return [0.0] * n
    r = random.Random(seed)
    amp = rms * (3 ** 0.5)  # 均匀分布配 rms
    return [r.uniform(-amp, amp) for _ in range(n)]


def build_stream_audio(seed=1, gap=1.0, n_noise=6, wake_pick=None, bg_rms=0.0012):
    """合成一条连续音流。返回 (samples, segments)
    segments = [{"start":样本, "end":样本, "is_wake":bool, "tag":str}, ...]
    只记录"语音"片段（不含 gap），按时间顺序。
    """
    r = random.Random(seed)
    segs = []
    buf = []

    def push(samples, is_wake, tag):
        st = len(buf)
        buf.extend(samples)
        segs.append({"start": st, "end": len(buf), "is_wake": is_wake, "tag": tag})

    # 开头 1.2s 底噪，让 VAD/端点有初始化空间
    buf.extend(silence(1.2, bg_rms, seed))

    items = [(False, p) for p in r.sample(NOISE_SRC, min(n_noise, len(NOISE_SRC)))]
    for name, (files, _keys) in WORDS.items():
        pool = wake_pick.get(name) if wake_pick else files
        for p in pool:
            items.append((True, p))
    r.shuffle(items)

    for is_wake, p in items:
        push(read_pcm(p), is_wake, os.path.basename(p))
        # 句间停顿：一半 gap，一半 2 倍 gap（制造不规则节奏）
        g = gap if r.random() < 0.6 else gap * 2
        buf.extend(silence(g, bg_rms, seed + len(segs)))
    buf.extend(silence(1.5, bg_rms, seed + 99))  # 结尾留够，逼出最后一句
    return buf, segs


def build_rec(threads=2, rule1=0.5, rule2=0.9, rule3=12.0, use_ep=True, model_dir="asr"):
    d = os.path.join(T, model_dir)
    f = lambda n: os.path.join(d, n)
    enc = [x for x in os.listdir(d) if x.startswith("encoder") and x.endswith(".onnx")][0]
    dec = [x for x in os.listdir(d) if x.startswith("decoder") and x.endswith(".onnx")][0]
    joi = [x for x in os.listdir(d) if x.startswith("joiner") and x.endswith(".onnx")][0]
    rec = sherpa_onnx.OnlineRecognizer.from_transducer(
        tokens=f("tokens.txt"), encoder=f(enc), decoder=f(dec), joiner=f(joi),
        num_threads=threads, sample_rate=SR, feature_dim=80,
        decoding_method="greedy_search", max_active_paths=4,
        enable_endpoint_detection=use_ep,
        rule1_min_trailing_silence=rule1,
        rule2_min_trailing_silence=rule2,
        rule3_min_utterance_length=rule3,
    )
    return rec


def run(samples, segs, rec, use_norm=True, fuzzy=True, collect_text=True):
    """分块流式喂入。返回 (utterances, wall_s, endpoint_count)"""
    st = rec.create_stream()
    utts = []
    n = len(samples)
    pos = 0
    t0 = time.perf_counter()
    while pos < n:
        blk = samples[pos:pos + CHUNK]
        st.accept_waveform(SR, blk)
        while rec.is_ready(st):
            rec.decode_stream(st)
        pos += len(blk)
        if rec.is_endpoint(st):
            r = rec.get_result(st)
            txt = r if isinstance(r, str) else r.text
            utts.append({"end_pos": pos, "text": txt})
            rec.reset(st)
    # 收尾：补静音把最后一句逼出来
    st.accept_waveform(SR, [0.0] * (SR * 2))
    st.input_finished()
    while rec.is_ready(st):
        rec.decode_stream(st)
    r = rec.get_result(st)
    tail = r if isinstance(r, str) else r.text
    if tail.strip():
        utts.append({"end_pos": n, "text": tail})
    wall = time.perf_counter() - t0

    # 把每个端点映射到它所属的语音片段：取"开始时间不晚于端点"的最后一个，
    # 即"当前正在播或刚播完的那句"。
    # 不能取"已结束的最后一个"——长静音里的空端点会跨过下一句的开头，
    # 把下一句的识别结果错记到上一句头上（实测过，会把整张表算错）。
    map_utts(utts, segs)
    return utts, wall, len(samples) / SR


def map_utts(utts, segs, use_norm=True, fuzzy=True):
    """端点 -> 语音片段映射 + 唤醒判定 + 响应延迟"""
    tag2keys = {}
    for _n, (files, k) in WORDS.items():
        for x in files:
            tag2keys[os.path.basename(x)] = k
    for u in utts:
        cand = [s for s in segs if s["start"] <= u["end_pos"]]
        s = cand[-1] if cand else None
        u["seg"] = s["tag"] if s else "-"
        u["is_wake_seg"] = bool(s and s["is_wake"])
        # 延迟：该句音频播完后，又等了多久才有结果（直接决定响应手感）
        # VAD 的 end_pos 落在语音自然结束处，可能早于 wav 尾部静音，故取 0 下限
        u["lat_s"] = round(max(0.0, (u["end_pos"] - s["end"]) / SR), 2) if s else None
        keys = tag2keys.get(u["seg"])
        u["hit"] = bool(keys and E.wake_hit(u["text"], keys, use_norm, fuzzy))
    return utts


def rms_of(blk):
    n = len(blk)
    if n == 0:
        return 0.0
    return (sum(float(v) * float(v) for v in blk) / n) ** 0.5


def run_vad(samples, segs, rec, k=3.0, preroll_ms=200, hang_ms=300, min_speech_ms=250):
    """能量 VAD 切段 + 每段独立识别（真实嵌入式主流形态）

    为什么不直接用 ASR 端点检测：实测长静音下端点每 rule1 秒反复触发，
    reset 之后仍在触发，会把下一句的音频切碎（16 句唤醒只认出 3 句）。
    VAD 只在有语音时才建流，从根上避开这个问题。

    必须留 preroll：语音起点前的一段音频也要送进去，否则首字被切掉
    （这里是 200ms，"你好"变"好"就是这么来的）。
    """
    pre_n = max(1, preroll_ms * SR // 1000 // CHUNK)
    hang_n = max(1, hang_ms * SR // 1000 // CHUNK)
    min_n = max(1, min_speech_ms * SR // 1000 // CHUNK)
    floor = rms_of(samples[:SR]) or 1e-4      # 开头 1s 是已知底噪，用它标定
    th = floor * k

    utts = []
    st = rec.create_stream()
    ring = []          # 最近 pre_n 块，用于 preroll
    in_speech = False
    sp_chunks = 0
    hang = 0
    pos = 0
    n = len(samples)
    peak_speech = 0.0

    def flush(end_pos):
        nonlocal st
        st.accept_waveform(SR, [0.0] * (SR * hang_ms // 1000 + SR // 10))
        st.input_finished()
        while rec.is_ready(st):
            rec.decode_stream(st)
        r = rec.get_result(st)
        utts.append({"end_pos": end_pos, "text": r if isinstance(r, str) else r.text})
        st = rec.create_stream()

    t0 = time.perf_counter()
    while pos < n:
        blk = samples[pos:pos + CHUNK]
        pos += CHUNK
        r_ = rms_of(blk)

        if r_ > th:
            if not in_speech:                 # 起点：补 preroll
                in_speech = True
                sp_chunks = 0
                for pb, _pp in ring:
                    st.accept_waveform(SR, pb)
                    while rec.is_ready(st):
                        rec.decode_stream(st)
            hang = hang_n
            sp_chunks += 1
            peak_speech = max(peak_speech, r_)
        elif in_speech:
            hang -= 1
            if hang <= 0:                     # 尾点
                if sp_chunks >= min_n:
                    flush(pos)
                else:
                    st = rec.create_stream()  # 太短，当噪声丢弃
                in_speech = False
                ring = []
                continue

        if in_speech:
            st.accept_waveform(SR, blk)
            while rec.is_ready(st):
                rec.decode_stream(st)

        ring.append((blk, pos))
        if len(ring) > pre_n:
            ring.pop(0)

    if in_speech:
        flush(n)
    wall = time.perf_counter() - t0
    utts_meta = {"floor": round(floor, 5), "th": round(th, 5), "peak": round(peak_speech, 5)}
    return utts, wall, len(samples) / SR, utts_meta


def run_vad_off(samples, segs, rec, k=3.0, preroll_ms=200, hang_ms=300, min_speech_ms=250):
    """VAD 切段 + 离线模型整段识别（嵌入式最可能落地的形态）

    流式模型为了低延迟牺牲了准确率；离线模型准确率高但需要先知道句子边界。
    VAD 正好提供边界，两者组合是常见做法。
    代价：必须等到句子结束才能出结果（延迟 = hangover），
          且离线模型算力需求高（60MB CTC 在 PC 上 RTF 0.024，A53 约 ×8~15）。
    """
    pre_n = max(1, preroll_ms * SR // 1000 // CHUNK)
    hang_n = max(1, hang_ms * SR // 1000 // CHUNK)
    min_n = max(1, min_speech_ms * SR // 1000 // CHUNK)
    floor = rms_of(samples[:SR]) or 1e-4
    th = floor * k

    utts = []
    cur = []           # 当前语音段累积的样本
    ring = []
    in_speech = False
    sp_chunks = 0
    hang = 0
    pos = 0
    n = len(samples)
    peak = 0.0

    def decode_seg(end_pos):
        pcm = list(cur)
        if not pcm:
            return
        r = E.decode(rec, pcm, SR)
        utts.append({"end_pos": end_pos, "text": r if isinstance(r, str) else r})

    t0 = time.perf_counter()
    while pos < n:
        blk = samples[pos:pos + CHUNK]
        pos += CHUNK
        r_ = rms_of(blk)

        if r_ > th:
            if not in_speech:
                in_speech = True
                sp_chunks = 0
                cur = [v for pb, _p in ring for v in pb]   # preroll
            hang = hang_n
            sp_chunks += 1
            peak = max(peak, r_)
        elif in_speech:
            hang -= 1
            if hang <= 0:
                if sp_chunks >= min_n:
                    decode_seg(pos)
                cur = []
                in_speech = False
                ring = []
                continue

        if in_speech:
            cur.extend(blk)

        ring.append((blk, pos))
        if len(ring) > pre_n:
            ring.pop(0)

    if in_speech and cur:
        decode_seg(n)

    wall = time.perf_counter() - t0
    return utts, wall, len(samples) / SR, {"floor": round(floor, 5), "th": round(th, 5), "peak": round(peak, 5)}


def summarize(utts, segs, wall, aud_s, rule1):
    n_wake_seg = sum(1 for s in segs if s["is_wake"])
    n_noise_seg = sum(1 for s in segs if not s["is_wake"])
    # 关键：一句语音结束后，长静音会让端点反复触发（每 rule1 秒一次），
    # 每次都是残句/空串。同一 seg 只保留"文本最完整"的那次，才是该句真实识别结果。
    # 真实部署靠 VAD 门控避免这个现象；不挂 VAD 时这是乐观上界。
    best = {}
    for u in utts:
        cur = best.get(u["seg"])
        if cur is None or len(u["text"].strip()) > len(cur["text"].strip()):
            best[u["seg"]] = u
        elif len(u["text"].strip()) == len(cur["text"].strip()) and cur["text"].strip() == "":
            best[u["seg"]] = u  # 都是空串，保留最后一条（延迟更接近真实触发）
    dedup = [best[s["tag"]] for s in segs if s["tag"] in best]
    n_dup = len(utts) - len(dedup)

    hit = sum(1 for u in dedup if u["is_wake_seg"] and u["hit"])
    fa = sum(1 for u in dedup if not u["is_wake_seg"] and u["hit"] and u["text"].strip())
    lats = [u["lat_s"] for u in dedup if u["is_wake_seg"] and u["lat_s"] is not None]
    # 按词分开统计：兮兮 与 小智 的可识别性差异极大，合在一起会掩盖结论
    by_word = {}
    for wname, (files, _keys) in WORDS.items():
        tags = set(os.path.basename(x) for x in files)
        sub = [u for u in dedup if u["seg"] in tags]
        by_word[wname] = [sum(1 for u in sub if u["hit"]), len(sub)]

    return dict(
        rule1=rule1,
        by_word=by_word,
        wake_seg=n_wake_seg,
        detected=sum(1 for u in dedup if u["is_wake_seg"]),
        hit=hit,
        detect_rate=round(sum(1 for u in dedup if u["is_wake_seg"]) / max(1, n_wake_seg) * 100, 1),
        wake_rate=round(hit / max(1, n_wake_seg) * 100, 1),
        noise_seg=n_noise_seg,
        fa=fa,
        fa_rate=round(fa / max(1, n_noise_seg) * 100, 1),
        n_utt=len(dedup),
        n_dup=n_dup,
        seg_total=len(segs),
        lat_avg=round(sum(lats) / len(lats), 2) if lats else None,
        lat_max=round(max(lats), 2) if lats else None,
        rtf=round(wall / max(1e-9, aud_s), 4),
        aud_s=round(aud_s, 1),
        utts=dedup,
    )


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--gap", type=float, default=1.0, help="句间平均停顿秒数")
    ap.add_argument("--rule1", default="0.6", help="端点模式的尾随静音阈值")
    ap.add_argument("--k", default="2.5,3.0,4.0", help="VAD 阈值 = 底噪 rms × k")
    ap.add_argument("--mode", default="endpoint,vad")
    ap.add_argument("--preroll", default="200", help="VAD 起点前回看的毫秒数（防首字被切）")
    ap.add_argument("--hang", default="300", help="VAD 尾点 hangover 毫秒数（防尾部丢字）")
    ap.add_argument("--off", default="", help="VAD+离线模型：指定模型目录，如 m_zipctc_s")
    ap.add_argument("--nnoise", type=int, default=6)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--json", default="")
    a = ap.parse_args()

    samples, segs = build_stream_audio(seed=a.seed, gap=a.gap, n_noise=a.nnoise)
    n_w = sum(1 for s in segs if s["is_wake"])
    print(f"连续音流：{len(samples)/SR:.1f}s，{len(segs)} 句"
          f"（唤醒 {n_w} / 无关 {len(segs)-n_w}），句间停顿均值 {a.gap}s")
    print(f"  -> 唤醒词真实出现 {n_w} 次，理想情况应检出 {n_w} 次、误唤醒 0 次\n")

    rows = []

    def report(tag, row, utts):
        rows.append(row)
        print(f"--- {tag} ---")
        bw = row.get("by_word", {})
        parts = "  ".join(f"{k} {v[0]}/{v[1]}({v[0]/max(1,v[1])*100:.0f}%)" for k, v in bw.items())
        print(f"  切出 {row['n_utt']}/{row['seg_total']} 句  判定命中 {row['hit']}/{row['wake_seg']}"
              f" ({row['wake_rate']}%)  误唤醒 {row['fa']}/{row['noise_seg']} ({row['fa_rate']}%)")
        print(f"  分词：{parts}")
        print(f"  延迟 均值 {row['lat_avg']}s 最大 {row['lat_max']}s   RTF {row['rtf']}")
        bad = [u for u in row["utts"] if u["is_wake_seg"] and not u["hit"]]
        if bad:
            print(f"  漏检 {len(bad)} 条：" + ", ".join(f"{u['seg']}→{u['text']!r}" for u in bad[:8]))
        print()

    if "endpoint" in a.mode:
        for r1 in [float(x) for x in a.rule1.split(",")]:
            rec = build_rec(rule1=r1, rule2=min(r1 * 1.8, 20), use_ep=True)
            utts, wall, aud = run(samples, segs, rec)
            row = summarize(utts, segs, wall, aud, f"ep{r1}")
            row["n_event"] = len(utts)
            print(f"[端点事件 {len(utts)} 次，去重后 {row['n_utt']} 句]")
            report(f"ASR 端点检测 rule1={r1}s", row, utts)

    if "vad" in a.mode:
        for k in [float(x) for x in a.k.split(",")]:
            for pre in [int(x) for x in a.preroll.split(",")]:
                for hg in [int(x) for x in a.hang.split(",")]:
                    rec = build_rec(use_ep=False)
                    utts, wall, aud, meta = run_vad(samples, segs, rec, k=k, preroll_ms=pre, hang_ms=hg)
                    map_utts(utts, segs)
                    row = summarize(utts, segs, wall, aud, f"vad{k}/pre{pre}/hang{hg}")
                    row.update(meta)
                    report(f"流式VAD k={k} preroll={pre}ms hang={hg}ms", row, utts)

    if a.off:
        for k in [float(x) for x in a.k.split(",")]:
            for pre in [int(x) for x in a.preroll.split(",")]:
                for hg in [int(x) for x in a.hang.split(",")]:
                    orec, _ld = E.build(a.off)
                    utts, wall, aud, meta = run_vad_off(samples, segs, orec, k=k, preroll_ms=pre, hang_ms=hg)
                    map_utts(utts, segs)
                    row = summarize(utts, segs, wall, aud, f"off{a.off}")
                    row.update(meta)
                    report(f"离线VAD[{a.off}] k={k} preroll={pre}ms hang={hg}ms", row, utts)

    if a.json:
        json.dump(rows, open(a.json, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        print("saved", a.json)


if __name__ == "__main__":
    main()
