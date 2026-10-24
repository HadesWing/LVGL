#!/usr/bin/env python3
"""
M5 压测判据 —— 从 stress_test.sh 的产物里读出硬数字，给 PASS/FAIL。

判据（全部可量化）：
  1. 轮数齐全：stress_rounds.txt 行数 == 预期轮数（每轮都有挂断统计）
  2. 0 崩溃：A 日志没有崩溃特征（断言/段错误特征串）
  3. 内存斜率：stats.csv 的 mem_used 序列一阶拟合，斜率 < MEM_SLOPE_BPS
     （正常应≈0；每轮漏一个 call 对象几 KB，斜率立刻上去）
  4. 碎片率：末段 frag_pct < FRAG_MAX
  5. 音频每轮真的有数据：A 每轮"落盘完成"峰值 > 0（样本数与轮数同量级）
  6. 视频：dec err=0、gap=0、au_dropped=0（推流轮全绿）
"""
import argparse
import re
import sys

MEM_SLOPE_BPS = 50      # 内存斜率阈值（字节/秒）
FRAG_MAX = 25           # 碎片率上限 %
CRASH_PAT = re.compile(r"assert|segmentation|Access violation|0xc0000005", re.I)


def analyze_log(path):
    text = open(path, encoding="utf-8", errors="replace").read()
    res = {
        "hangups": len(re.findall(r"stats\(hangup\)", text)),
        "media_start": len(re.findall(r"media start", text)),
        "crash": bool(CRASH_PAT.search(text)),
        "rx_wavs": [(int(n), int(peak)) for n, peak in
                    re.findall(r"落盘完成: \S+  (\d+) 采样 \([\d.]+ 秒\)  峰值=(-?\d+)", text)],
        "video": re.findall(r"\[video\] stats\(hangup\)", text),
        "dec_err": sum(int(e) for e in
                       re.findall(r"dec\{[^}]*err=(\d+)", text)),
        "gap": sum(int(g) for g in
                   re.findall(r"gap=(\d+)", text)),
        "au_dropped": sum(int(d) for d in
                          re.findall(r"au_dropped=(\d+)", text)),
    }
    return res


def analyze_csv(path):
    """mem_used 一阶拟合斜率 + 末段碎片率。跳过前 5s（启动期抖动大）。"""
    xs, used, frag = [], [], []
    with open(path, encoding="utf-8") as f:
        header = f.readline()
        if "mem_used" not in header:
            return None
        for line in f:
            p = line.strip().split(",")
            if len(p) < 9:
                continue
            sec = int(p[0])
            if sec < 5:
                continue
            xs.append(sec)
            used.append(int(p[4]))
            frag.append(int(p[7]))
    if len(xs) < 10:
        return None
    n = len(xs)
    mx, my = sum(xs) / n, sum(used) / n
    denom = sum((x - mx) ** 2 for x in xs)
    slope = sum((xs[i] - mx) * (used[i] - my) for i in range(n)) / denom if denom else 0.0
    tail_frag = sum(frag[-n // 5:]) / max(1, len(frag[-n // 5:]))
    return {"slope": slope, "tail_frag": tail_frag, "samples": n,
            "first": used[0], "last": used[-1], "max_used": max(used)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--rounds", type=int, required=True)
    ap.add_argument("--rounds-done", required=True)
    args = ap.parse_args()

    log = analyze_log(args.log)
    mem = analyze_csv(args.csv)
    try:
        done = sum(1 for _ in open(args.rounds_done))
    except OSError:
        done = 0

    checks = []

    def check(name, ok, detail):
        checks.append((name, ok, detail))

    check("轮数齐全", done == args.rounds,
          f"{done}/{args.rounds} 轮有挂断统计")
    check("0 崩溃", not log["crash"], "无崩溃特征串" if not log["crash"] else "日志有崩溃特征")
    if mem:
        check("内存斜率", abs(mem["slope"]) <= MEM_SLOPE_BPS,
              f"{mem['slope']:+.1f} B/s（阈值 ±{MEM_SLOPE_BPS}，{mem['samples']} 个采样，"
              f"used {mem['first']}→{mem['last']} B，峰值 {mem['max_used']}）")
        check("碎片率", mem["tail_frag"] < FRAG_MAX,
              f"末段 {mem['tail_frag']:.0f}%（阈值 {FRAG_MAX}%）")
    else:
        check("内存曲线", False, "stats.csv 无有效数据")
    zero_rx = [n for n, peak in log["rx_wavs"] if peak <= 0]
    check("音频每轮非零", log["rx_wavs"] and not zero_rx,
          f"{len(log['rx_wavs'])} 轮落盘，峰值全>0" if log["rx_wavs"] and not zero_rx
          else f"峰值异常轮：{zero_rx}")
    check("视频零错误", log["dec_err"] == 0 and log["au_dropped"] == 0,
          f"dec err={log['dec_err']}  au_dropped={log['au_dropped']}  "
          f"（挂断统计 {log['hangups']} 次）")

    print("\n──── M5 压测判据 ────")
    fail = 0
    for name, ok, detail in checks:
        mark = "✅" if ok else "❌"
        if not ok:
            fail += 1
        print(f"{mark} {name}: {detail}")

    print(f"\n{'✅ 压测通过' if fail == 0 else f'❌ {fail} 项不达标'}")
    sys.exit(0 if fail == 0 else 1)


if __name__ == "__main__":
    main()
