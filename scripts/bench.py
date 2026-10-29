"""唤醒方案全矩阵：模型 × 后处理档位，输出 CSV 与 Markdown 表

跑法: python scripts/bench.py
"""
import sys, os, json, csv
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval as E

T = E.T
OFFLINE = ["m_zipctc_s", "m_dolphin", "asr_sv", "m_paraformer"]
STREAM = ["asr"]
LEVELS = [("原始输出", dict(use_norm=False, fuzzy=False)),
          ("同音归一", dict(use_norm=True, fuzzy=False)),
          ("+近音容错", dict(use_norm=True, fuzzy=True))]

def one_stream(d, **kw):
    return E.run_wake_stream(d, "", 1.5, None, **kw)

def one_offline(d, **kw):
    return E.run_wake(d, "", 1.5, None, "", **kw)

def row(name, res, level):
    w = {x["word"]: x for x in res["words"]}
    a, b = w.get("兮兮", {}), w.get("小智", {})
    return dict(model=name, level=level, mb=res["mb"],
                xixi=f"{a.get('hit',0)}/{a.get('n',0)}", xixi_pct=a.get("rate", 0),
                xz=f"{b.get('hit',0)}/{b.get('n',0)}", xz_pct=b.get("rate", 0),
                rtf=min(a.get("rtf", 0), 0) or a.get("rtf", 0),
                rtf2=b.get("rtf", 0), peak=res["peak_mb"],
                texts={"兮兮": a.get("texts", []), "小智": b.get("texts", [])})

if __name__ == "__main__":
    rows = []
    for d in OFFLINE:
        if not os.path.isdir(os.path.join(T, d)):
            continue
        for lv, kw in LEVELS:
            try:
                r = one_offline(d, **kw)
                rows.append(row(d, r, lv)); print(f"ok  {d:<14}{lv}")
            except Exception as e:
                print(f"ERR {d} {lv}: {e}")
    for d in STREAM:
        for lv, kw in LEVELS:
            try:
                r = one_stream(d, **kw)
                rows.append(row(d + "(stream)", r, lv)); print(f"ok  {d}(stream) {lv}")
            except Exception as e:
                print(f"ERR {d} stream {lv}: {e}")

    with open(os.path.join(T, "wake_matrix.json"), "w", encoding="utf-8") as f:
        json.dump(rows, f, ensure_ascii=False, indent=1)

    print("\n| 模型 | 体积MB | 峰值MB | 后处理 | 兮兮(12) | 小智(4) |")
    print("|---|---|---|---|---|")
    for r in rows:
        print(f"| {r['model']} | {r['mb']:.0f} | {r['peak']:.0f} | {r['level']} | "
              f"{r['xixi']} ({r['xixi_pct']:.0f}%) | {r['xz']} ({r['xz_pct']:.0f}%) |")
