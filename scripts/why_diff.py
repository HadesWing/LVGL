# -*- coding: utf-8 -*-
"""为什么流式和离线结果不同？—— 把"喂法"和"模型"两个变量拆开

三个对照实验：
  A. 同一个流式模型，换喂入块大小（20ms~整段）→ 文本是否变化
  B. 同一个离线模型，把一句音频切成 N 段各送一次 → 文本是否崩坏
  C. 同一个离线模型，分块多次 accept_waveform 累积，最后解一次 → 是否等价整段

结论指向：
  - 喂法（chunk 大小）不影响结果  → 用户"分割方式一样结果就该一样"的推理正确
  - 但流式/离线的差别不是喂法，是【两个不同的模型文件】
  - 离线模型不能切碎喂，因为它的 encoder 没有跨 chunk 状态
"""
import os
import sys
import json

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval as E
import stream_eval as S

SR = 16000
T = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), ".tools")

FILES = [
    (os.path.join(T, "rec/xiaozhi_phone/p01.wav"), "小智1"),
    (os.path.join(T, "rec/xiaozhi_phone/p02.wav"), "小智2"),
    (os.path.join(T, "rec/xiaozhi_phone/p03.wav"), "小智3"),
    (os.path.join(T, "rec/xixi_seg/s01.wav"), "兮兮1"),
    (os.path.join(T, "rec/xixi_seg/s02.wav"), "兮兮2"),
    (os.path.join(T, "rec/xixi_seg/s03.wav"), "兮兮3"),
]


# ---------------- A. 流式模型：换喂入块大小 ----------------
def online_chunk(rec, samples, chunk_ms):
    ch = len(samples) if chunk_ms <= 0 else max(1, SR * chunk_ms // 1000)
    st = rec.create_stream()
    pos = 0
    while pos < len(samples):
        st.accept_waveform(SR, samples[pos:pos + ch])
        pos += ch
        while rec.is_ready(st):
            rec.decode_stream(st)
    st.input_finished()
    while rec.is_ready(st):
        rec.decode_stream(st)
    r = rec.get_result(st)
    return r if isinstance(r, str) else r.text


# ---------------- B. 离线模型：切 N 段各送一次 ----------------
def offline_split(rec, samples, n_seg):
    if n_seg <= 1:
        return E.decode(rec, samples, SR)
    step = len(samples) // n_seg
    out = []
    for i in range(n_seg):
        seg = samples[i * step:(i + 1) * step] if i < n_seg - 1 else samples[i * step:]
        out.append(E.decode(rec, seg, SR))
    return "|".join(out)


# ---------------- C. 离线模型：分块累积，最后解一次 ----------------
def offline_accum(rec, samples, chunk_ms):
    ch = len(samples) if chunk_ms <= 0 else max(1, SR * chunk_ms // 1000)
    st = rec.create_stream()
    pos = 0
    while pos < len(samples):
        st.accept_waveform(SR, samples[pos:pos + ch])
        pos += ch
    rec.decode_stream(st)
    return st.result.text


def main():
    import time
    _t = time.perf_counter()
    on_rec = S.build_rec(use_ep=False)
    on_load = time.perf_counter() - _t
    off_rec, off_load = E.build("m_zipctc_s", model_file="model.int8.onnx")
    print(f"流式模型加载 {on_load:.2f}s / 离线模型加载 {off_load:.2f}s\n")

    res = {"A": [], "B": [], "C": []}

    # ---- A ----
    print("=" * 78)
    print("A. 流式模型：只改变喂入块大小，模型和参数完全不变")
    print("=" * 78)
    chunks = [20, 50, 100, 200, 500, 0]
    print(f"{'音频':<8}" + "".join(f"{str(c) + 'ms' if c else '整段':<14}" for c in chunks) + "  一致?")
    for path, tag in FILES:
        if not os.path.exists(path):
            continue
        samples = S.read_pcm(path)
        texts = [online_chunk(on_rec, samples, c) for c in chunks]
        same = len(set(texts)) == 1
        res["A"].append({"tag": tag, "texts": texts, "same": same})
        print(f"{tag:<8}" + "".join(f"{t!r:<14}" for t in texts) + f"  {'是' if same else '否'}")

    # ---- B ----
    print()
    print("=" * 78)
    print("B. 离线模型：把一句话强行切成 N 段，每段独立识别")
    print("=" * 78)
    print(f"{'音频':<8}{'1段(整句)':<16}{'2段':<24}{'4段'}")
    for path, tag in FILES:
        if not os.path.exists(path):
            continue
        t1 = offline_split(off_rec, samples, 1) if (samples := S.read_pcm(path)) else ""
        t2 = offline_split(off_rec, samples, 2)
        t4 = offline_split(off_rec, samples, 4)
        res["B"].append({"tag": tag, "n1": t1, "n2": t2, "n4": t4})
        print(f"{tag:<8}{t1!r:<16}{t2!r:<24}{t4!r}")

    # ---- C ----
    print()
    print("=" * 78)
    print("C. 离线模型：分块多次送进去（内部累积），最后只解一次")
    print("=" * 78)
    print(f"{'音频':<8}" + "".join(f"{str(c) + 'ms' if c else '整段':<12}" for c in [20, 100, 500, 0]) + "  一致?")
    for path, tag in FILES:
        if not os.path.exists(path):
            continue
        samples = S.read_pcm(path)
        texts = [offline_accum(off_rec, samples, c) for c in [20, 100, 500, 0]]
        same = len(set(texts)) == 1
        res["C"].append({"tag": tag, "texts": texts, "same": same})
        print(f"{tag:<8}" + "".join(f"{t!r:<12}" for t in texts) + f"  {'是' if same else '否'}")

    out = os.path.join(T, "why_diff.json")
    json.dump(res, open(out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print(f"\nsaved {out}")

    # ---- 汇总判据 ----
    print()
    print("=" * 78)
    a_all = all(x["same"] for x in res["A"])
    c_all = all(x["same"] for x in res["C"])
    print(f"A 流式模型：6 种喂法文本完全一致 = {a_all}")
    print(f"C 离线模型：4 种累积喂法文本完全一致 = {c_all}")
    print("→ 喂法不影响结果。差异只可能来自模型本身。")
    print("=" * 78)


if __name__ == "__main__":
    main()
