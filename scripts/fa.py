"""误唤醒（False Accept）测试：用 20 句无关指令 + 诱导音，看唤醒判定会不会误触发

跑法: python scripts/fa.py [模型目录...]
"""
import sys, os, json
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval as E

T = E.T
# 负样本：20 句智能家居指令（与唤醒词无关）
NEG_DIR = os.path.join(T, "cer_set")
# 诱导负样本：其他常见唤醒词（如存在）
INDUCE = ["小爱小爱", "小度小度", "你好天猫", "天猫精灵", "你好小艺", "小艺小艺"]


def load_neg():
    out = []
    with open(os.path.join(NEG_DIR, "trans.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            w, ref = line.split("|", 1)
            out.append((os.path.join(NEG_DIR, w), ref))
    return out


def run(name, stream=False):
    if stream:
        rec, load_s = E.build_stream(name)
        dec = E.decode_stream
    else:
        rec, load_s = E.build(name)
        dec = E.decode
    pos_keys = ["你好兮兮", "兮兮", "你好小智", "小智"]
    rows = []
    for p, ref in load_neg():
        s, sr = E.read_wav(p)
        txt = dec(rec, s)
        fa = E.wake_hit(txt, pos_keys, True, True)
        rows.append((ref, txt, fa))
    n_fa = sum(1 for r in rows if r[2])
    print(f"{name:<16} 负样本 {len(rows)} 句  误唤醒 {n_fa} 次 ({n_fa/len(rows)*100:.1f}%)  峰值{E.peak_rss_mb()[0]:.0f}MB")
    for ref, txt, fa in rows:
        if fa:
            print(f"   ! 误触发: 参考[{ref}] 识别[{txt}]")
    return rows


if __name__ == "__main__":
    targets = sys.argv[1:] or ["m_zipctc_s", "asr_sv", "m_paraformer", "asr"]
    allr = {}
    for t in targets:
        try:
            allr[t] = run(t, stream=(t == "asr"))
        except Exception as e:
            print(f"ERR {t}: {e}")
    json.dump(allr, open(os.path.join(T, "false_accept.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)
