#!/usr/bin/env python3
"""端侧 ASR 评测台：CER / RTF / 内存峰值。

回答的问题不是「这个模型准不准」，而是「换到我的设备上，够不够用」。

指标定义：
  CER  = 字级编辑距离 / 参考文本字数（中文按字算，去掉标点空格）
         < 5%  可用；5~15% 需要容错解析；> 15% 不可用
  RTF  = 推理耗时 / 音频时长。端侧要 < 0.5 才能实时（留一半余量给业务）
  内存 = 加载后的工作集（Windows 走 psapi，其他平台退化成 -1）

测试集格式：一个目录，内含
  *.wav        16k 单声道（脚本会自己转）
  trans.txt    每行 `文件名|参考文本`，例如  01.wav|打开客厅的灯

用法：
  python scripts/asr_cer.py --models ../../.tools/asr_sv ../../.tools/m_dolphin \
      --set ../../.tools/cer_set --json ../../.tools/cer.json
"""

import argparse
import json
import os
import re
import sys
import tempfile
import time
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kws_test as K          # noqa: E402
import asr_compare as AC      # noqa: E402

PUNCT = re.compile(r"[\s，。！？、,.!?：:；;\"'“”‘’（）()《》\-—_…~]+")


def norm(s):
    return PUNCT.sub("", s or "")


def edit_distance(a, b):
    """字级 Levenshtein，O(len(a)*len(b))，中文句子够用。"""
    if not a:
        return len(b)
    if not b:
        return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i] + [0] * len(b)
        for j, cb in enumerate(b, 1):
            cur[j] = min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb))
        prev = cur
    return prev[-1]


def mem_mb():
    """当前进程工作集（MB）。

    ctypes 走 GetProcessMemoryInfo 在部分环境下拿不到（结构体/调用约定差异），
    改用 tasklist CSV 输出来抓，Windows 下最稳。
    """
    if os.name != "nt":
        return -1.0
    try:
        import subprocess
        out = subprocess.check_output(
            ["tasklist", "/FI", "PID eq %d" % os.getpid(), "/FO", "CSV", "/NH"],
            stderr=subprocess.DEVNULL).decode("mbcs", "ignore")
        # "python.exe","1234","Console","1","123,456 K"
        # 坑：内存字段自带千位分隔符逗号，按 "," split 会被切成两段，
        # 导致读出一个几十 KB 的假数字。必须按 `"数字,数字 K"` 整段匹配。
        m = re.search(r'"([\d,]+) K"', out)
        if m:
            return float(m.group(1).replace(",", "")) / 1024.0
    except Exception:
        pass
    return -1.0


def load_set(d):
    trans = os.path.join(d, "trans.txt")
    if not os.path.isfile(trans):
        sys.exit("测试集缺少 trans.txt: " + trans)
    items = []
    with open(trans, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or "|" not in line:
                continue
            name, ref = line.split("|", 1)
            p = os.path.join(d, name.strip())
            if os.path.isfile(p):
                items.append((p, name.strip(), ref.strip()))
            else:
                print("  [警告] 找不到 %s，跳过" % name)
    if not items:
        sys.exit("测试集为空")
    return items


def run_model(md, items, threads=2, verbose=False):
    """返回该模型的指标字典。加载耗时不算进 RTF。"""
    base = mem_mb()
    t0 = time.perf_counter()
    rec, meta = AC.build(md)
    load_s = time.perf_counter() - t0
    after = mem_mb()

    rows = []
    ref_chars = 0
    err_chars = 0
    audio_s = 0.0
    infer_s = 0.0
    exact = 0
    for path, name, ref in items:
        pcm, rate = K.read_wav_mono16k(path)
        x = np.asarray(pcm, dtype=np.float32) / 32768.0
        dur = len(x) / float(rate)
        t1 = time.perf_counter()
        s = rec.create_stream()
        s.accept_waveform(rate, x)
        rec.decode_stream(s)
        dt = time.perf_counter() - t1
        text = (s.result.text or "").strip()

        r, h = norm(ref), norm(text)
        d = edit_distance(r, h)
        ref_chars += len(r)
        err_chars += d
        audio_s += dur
        infer_s += dt
        exact += 1 if r == h else 0
        rows.append((name, dur, dt, ref, text, (100.0 * d / len(r)) if r else 0.0))
        if verbose:
            print("      %-10s %5.2fs  CER %5.1f%%  ref=%s  hyp=%s"
                  % (name, dur, (100.0 * d / len(r)) if r else 0.0, r, h))

    return {
        "model": os.path.basename(os.path.normpath(md)),
        "kind": meta.get("kind", "?"),
        "n": len(items),
        "cer": 100.0 * err_chars / ref_chars if ref_chars else 0.0,
        "exact": exact,
        "rtf": infer_s / audio_s if audio_s else 0.0,
        "load_s": load_s,
        "mem_mb": after,
        "mem_delta_mb": (after - base) if after > 0 else -1.0,
        "rows": rows,
    }


def make_snr_set(items, snr_db):
    """把测试集按给定 SNR 混入白噪，写到临时目录并返回路径。

    端侧真实场景几乎不可能是干净的，用干净 TTS 测出来的 CER 会严重高估。
    噪声档位才是区分模型鲁棒性的关键指标 —— 这也是「端侧能力怎么评估」的核心。
    """
    d = os.path.join(tempfile.gettempdir(), "cer_snr%g" % snr_db)
    os.makedirs(d, exist_ok=True)
    rng = np.random.RandomState(1234)     # 固定种子，保证各模型听到同一份噪声
    lines = []
    for path, name, ref in items:
        pcm, rate = K.read_wav_mono16k(path)
        x = np.asarray(pcm, dtype=np.float64)
        p = float(np.mean(x ** 2))
        n = rng.randn(len(x))
        n *= np.sqrt(p / (10.0 ** (snr_db / 10.0))) / np.sqrt(float(np.mean(n ** 2)))
        y = np.clip(x + n, -32768, 32767).astype(np.int16)
        p2 = os.path.join(d, name)
        with wave.open(p2, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(y.tobytes())
        lines.append("%s|%s" % (name, ref))
    with open(os.path.join(d, "trans.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--models", nargs="+", required=True)
    ap.add_argument("--set", required=True, help="测试集目录")
    ap.add_argument("--snr", default="", help="噪声扫描，如 20,10,5（省略=不加噪）")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--json", default="", help="把结果写成 json")
    a = ap.parse_args()

    items = load_set(a.set)
    print("测试集 %s：%d 条，参考文本共 %d 字"
          % (a.set, len(items), sum(len(norm(r)) for _, _, r in items)))
    print()

    snrs = [float(s) for s in a.snr.split(",")] if a.snr else [None]
    sets = {}
    for s in snrs:
        sets[s] = items if s is None else load_set(make_snr_set(items, s))

    out = []
    for md in a.models:
        if not os.path.isdir(md):
            print("跳过（不是目录）:", md)
            continue
        for s in snrs:
            try:
                r = run_model(md, sets[s], verbose=a.verbose)
            except Exception as e:
                print("[%s @%s] 失败: %s" % (os.path.basename(md), s, e))
                continue
            r["snr"] = s
            out.append(r)
            print("  %-14s %6s  CER %6.2f%%  全对 %2d/%2d  RTF %.3f  加载 %5.2fs  RSS %5.0fMB"
                  % (r["model"], "干净" if s is None else "%.0fdB" % s, r["cer"],
                     r["exact"], r["n"], r["rtf"], r["load_s"], r["mem_mb"]))

    if not out:
        sys.exit("没有任何模型跑成功")

    print()
    if len(snrs) > 1:
        uniq = []
        for r in out:
            if r["model"] not in uniq:
                uniq.append(r["model"])
        print("=" * 78)
        print("CER 随噪声变化（越小越好）")
        print("%-16s" % "模型" + "".join("%10s" % ("干净" if s is None else "%.0fdB" % s) for s in snrs))
        print("-" * 78)
        for nm in uniq:
            row = "%-16s" % nm
            for s in snrs:
                v = [r for r in out if r["model"] == nm and r["snr"] == s]
                row += "%10s" % ("%.1f%%" % v[0]["cer"] if v else "-")
            print(row)
        print("=" * 78)
        print("端侧要看的是「10dB 那一列」—— 那是远场/有背景音的常态。")
        print("干净列全对、10dB 崩掉的模型，在设备上等于不能用。")
    else:
        print("=" * 78)
        print("%-16s %8s %10s %8s %10s %10s" % ("模型", "CER", "全对", "RTF", "加载s", "RSS MB"))
        print("-" * 78)
        for r in sorted(out, key=lambda x: x["cer"]):
            print("%-16s %7.2f%% %8d/%-2d %8.3f %9.2f %10.0f"
                  % (r["model"], r["cer"], r["exact"], r["n"], r["rtf"],
                     r["load_s"], r["mem_mb"]))
        print("=" * 78)
        print("判定参考：CER <5% 直接可用 / 5~15% 需容错解析 / >15% 不可用")
        print("          RTF <0.5 可实时 / 0.5~1.0 勉强 / >1.0 跟不上（PC 数据要乘板端系数）")

    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(out, f, ensure_ascii=False, indent=1)
        print("\n已写入 %s" % a.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
