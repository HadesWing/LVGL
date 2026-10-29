"""端侧 ASR 评测脚手架（纯标准库 + sherpa_onnx，无需 numpy）

用法:
  python scripts/eval.py scan                    # 列出可评模型
  python scripts/eval.py cer  <model_dir>        # 指令集 CER + RTF + 峰值内存
  python scripts/eval.py wake <model_dir>        # 唤醒词检出（兮兮12句 / 小智4句）
  python scripts/eval.py quant                   # 量化三档对照（zipctc fp32/官方int8/自量化）
  python scripts/eval.py hot <model_dir>         # transducer 热词提权扫描
可选: --snr 20,10,5    --json out.json
"""
import sys, os, io, wave, array, time, json, math, random, argparse, ctypes

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T = os.path.join(ROOT, ".tools")
import sherpa_onnx

PUNCT = "，。！？、,.!? ；;：:\"'“”‘’（）()·…—-"

# ---------------- 音频 IO（标准库） ----------------
def read_wav(path):
    with wave.open(path, "rb") as f:
        sr, n, ch, sw = f.getframerate(), f.getnframes(), f.getnchannels(), f.getsampwidth()
        raw = f.readframes(n)
    a = array.array("h")
    a.frombytes(raw)
    if ch > 1:
        a = array.array("h", a[::ch])
    if sr != 16000:
        # 简单线性重采样
        dst = int(len(a) * 16000 / sr)
        out = array.array("h", [0] * dst)
        for i in range(dst):
            p = i * sr / 16000.0
            i0 = int(p); i1 = min(i0 + 1, len(a) - 1)
            out[i] = int(a[i0] + (a[i1] - a[i0]) * (p - i0))
        a, sr = out, 16000
    return [v / 32768.0 for v in a], sr

def add_noise(samples, snr_db, seed=1234):
    """按指定 SNR 叠加白噪声（同种子可复现）"""
    rnd = random.Random(seed)
    p_sig = sum(v * v for v in samples) / max(1, len(samples))
    if p_sig <= 0:
        return samples
    p_noise = p_sig / (10 ** (snr_db / 10.0))
    sigma = math.sqrt(p_noise)
    out = []
    for v in samples:
        # 近似高斯：12 个均匀分布求和
        g = sum(rnd.random() for _ in range(12)) - 6.0
        out.append(max(-1.0, min(1.0, v + g * sigma)))
    return out

# ---------------- 峰值内存（Windows） ----------------
class PMC(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong),
                ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

def peak_rss_mb():
    """返回 (峰值工作集, 当前工作集) MB；失败为 -1"""
    try:
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.GetCurrentProcess.restype = ctypes.c_void_p
        k32.K32GetProcessMemoryInfo.argtypes = [ctypes.c_void_p, ctypes.POINTER(PMC), ctypes.c_ulong]
        k32.K32GetProcessMemoryInfo.restype = ctypes.c_int
        pmc = PMC(); pmc.cb = ctypes.sizeof(PMC)
        ok = k32.K32GetProcessMemoryInfo(k32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
        if not ok:
            return -1.0, -1.0
        return pmc.PeakWorkingSetSize / 1048576.0, pmc.WorkingSetSize / 1048576.0
    except Exception:
        return -1.0, -1.0

# ---------------- 模型构建 ----------------
def build(d, hotwords_file="", hotwords_score=1.5, threads=2, model_file=""):
    d = os.path.join(T, d) if not os.path.isabs(d) else d
    f = lambda *p: os.path.join(d, *p)
    ents = set(os.listdir(d))
    t0 = time.perf_counter()
    if "model.int8.onnx" in ents or "model.onnx" in ents:
        m = f(model_file) if model_file else (f("model.int8.onnx") if "model.int8.onnx" in ents else f("model.onnx"))
        tok = f("tokens.txt")
        if "bbpe.model" in ents or m.endswith("model.onnx") and os.path.getsize(m) < 3e8:
            # zipformer-ctc 单文件（小尺寸）走 ctc，大尺寸走 paraformer 分支判断
            pass
        kind = _sniff_ctc_or_para(m)
        if kind == "paraformer":
            r = sherpa_onnx.OfflineRecognizer.from_paraformer(paraformer=m, tokens=tok, num_threads=threads)
        elif kind == "dolphin":
            r = sherpa_onnx.OfflineRecognizer.from_dolphin_ctc(model=m, tokens=tok, num_threads=threads)
        elif kind == "sensevoice":
            r = sherpa_onnx.OfflineRecognizer.from_sense_voice(model=m, tokens=tok, num_threads=threads, use_itn=True)
        else:
            r = sherpa_onnx.OfflineRecognizer.from_zipformer_ctc(model=m, tokens=tok, num_threads=threads)
    elif "encoder_model.ort" in ents:
        r = sherpa_onnx.OfflineRecognizer.from_moonshine_v2(
            preprocessor=f("preprocess.onnx") if "preprocess.onnx" in ents else "",
            encoder=f("encoder_model.ort"), decoder=f("decoder_model_merged.ort"),
            tokens=f("tokens.txt"), num_threads=threads)
    elif "encoder-epoch-99-avg-1.int8.onnx" in ents:
        r = sherpa_onnx.OfflineRecognizer.from_transducer(
            encoder=f("encoder-epoch-99-avg-1.int8.onnx"),
            decoder=f("decoder-epoch-99-avg-1.int8.onnx"),
            joiner=f("joiner-epoch-99-avg-1.int8.onnx"),
            tokens=f("tokens.txt"), num_threads=threads,
            hotwords_file=hotwords_file, hotwords_score=hotwords_score,
            modeling_unit="cjkchar", bpe_vocab=f("bbpe.model") if "bbpe.model" in ents else "")
    else:
        raise SystemExit(f"不认识这个目录结构: {d}")
    return r, time.perf_counter() - t0

def _sniff_ctc_or_para(path):
    """按文件名/体积粗判：sensevoice≈239M、paraformer≈238M 均需区分，靠文件名前缀不靠谱，
    统一用 onnx 里的元信息不可行（无 onnx 库），改用体积+模型目录名启发。"""
    name = os.path.basename(os.path.dirname(path)).lower()
    if "sv" in name or "sense" in name:
        return "sensevoice"
    if "para" in name:
        return "paraformer"
    if "dolphin" in name:
        return "dolphin"
    return "ctc"

def size_mb(d, model_file=""):
    d = os.path.join(T, d) if not os.path.isabs(d) else d
    if model_file:
        return os.path.getsize(os.path.join(d, model_file)) / 1048576.0
    return sum(os.path.getsize(os.path.join(d, x)) for x in os.listdir(d)
               if os.path.isfile(os.path.join(d, x))) / 1048576.0

# ---------------- 指标 ----------------
def norm(s):
    return "".join(c for c in s if c not in PUNCT and not c.isspace())

def edit_dist(a, b):
    if not a: return len(b)
    if not b: return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]

def cer_of(ref, hyp):
    ref, hyp = norm(ref), norm(hyp)
    return edit_dist(ref, hyp) / max(1, len(ref)) * 100.0

def decode(rec, samples, sr=16000):
    s = rec.create_stream()
    s.accept_waveform(sr, samples)
    rec.decode_stream(s)
    return s.result.text

# ---------------- 任务 ----------------
def run_cer(dirname, snr_list=(None,), json_out=None, model_file=""):
    rec, load_s = build(dirname, model_file=model_file)
    items = []
    with open(os.path.join(T, "cer_set", "trans.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line: continue
            w, ref = line.split("|", 1)
            items.append((w, ref))
    rows_all = []
    for snr in snr_list:
        rows, t_infer, t_audio = [], 0.0, 0.0
        for w, ref in items:
            s, sr = read_wav(os.path.join(T, "cer_set", w))
            if snr is not None:
                s = add_noise(s, snr, seed=hash(w) & 0xffff)
            t0 = time.perf_counter(); hyp = decode(rec, s, sr); t_infer += time.perf_counter() - t0
            t_audio += len(s) / sr
            rows.append((w, ref, hyp, round(cer_of(ref, hyp), 2)))
        cer = sum(r[3] for r in rows) / len(rows)
        exact = sum(1 for r in rows if r[3] == 0)
        rec_ = dict(tag=("clean" if snr is None else f"{snr}dB"), model=os.path.basename(dirname.rstrip("\\/")),
                    mb=round(size_mb(dirname, model_file), 1), load_s=round(load_s, 2),
                    cer=round(cer, 2), exact=exact, n=len(rows),
                    rtf=round(t_infer / max(1e-9, t_audio), 4), peak_mb=round(peak_rss_mb()[0], 1))
        rows_all.append((rec_, rows))
    if json_out:
        with open(json_out, "w", encoding="utf-8") as f:
            json.dump([{"sum": r, "rows": rows} for r, rows in rows_all], f, ensure_ascii=False, indent=1)
    return rows_all


# ---------------- 同音字归一（工程后处理，可 1:1 搬到 C） ----------------
HOMO = [  # 裸替换：这些串日常不会独立出现，替换安全
    # "您"->"你" 必须排第一：流式切分常把首字改成"您好"，
    # 后面所有以"你好"为锚点的近音规则就全失配了（实测漏检 p02/p03 就是这个）。
    ("您好", "你好"),
    ("西西", "兮兮"), ("嘻嘻", "兮兮"), ("希希", "兮兮"), ("夕夕", "兮兮"),
    ("溪溪", "兮兮"), ("熙熙", "兮兮"), ("细细", "兮兮"), ("习习", "兮兮"),
    ("西嘻", "兮兮"),
    ("小致", "小智"), ("小治", "小智"), ("小志", "小智"), ("小知", "小智"),
    ("小至", "小智"), ("小制", "小智"), ("小值", "小智"), ("小只", "小智"),
    ("小止", "小智"), ("晓智", "小智"), ("筱智", "小智"), ("小秩", "小智"),
]
# 近音：必须带"你好/你行"锚点，否则"我想吃饭"会把"想吃"replace 成"小智"→ 误唤醒
NEAR = [
    ("你好小吃", "你好小智"), ("你好想吃", "你好小智"), ("你好小气", "你好小智"),
    ("你好谢谢", "你好兮兮"), ("你行嘻嘻", "你好兮兮"), ("你好星星", "你好兮兮"),
    ("你好谢西", "你好兮兮"), ("你好学习", "你好兮兮"),
]

def homo_norm(t, fuzzy=False):
    for a, b in (HOMO + NEAR if fuzzy else HOMO):
        t = t.replace(a, b)
    return t

WAKE = {"兮兮": ([os.path.join(T, "rec", "xixi_seg", f"s{i:02d}.wav") for i in range(1, 13)], ["你好兮兮", "兮兮"]),
        "小智": ([os.path.join(T, "rec", "xiaozhi_phone", f"p{i:02d}.wav") for i in range(1, 5)], ["你好小智", "你好，小智", "小智"])}

def wake_hit(text, keys, use_norm=False, fuzzy=False):
    # 必须先去标点再归一，否则"你好，谢谢"匹配不上"你好谢谢"锚点规则
    n = norm(text)
    if use_norm:
        n = homo_norm(n, fuzzy)
    return any(norm(k) in n for k in keys)

def run_wake(dirname, hotwords_file="", hotwords_score=1.5, json_out=None, model_file="", use_norm=False, fuzzy=False):
    rec, load_s = build(dirname, hotwords_file, hotwords_score, model_file=model_file)
    out = []
    for name, (files, keys) in WAKE.items():
        hit, texts, t_inf, t_aud = 0, [], 0.0, 0.0
        for p in files:
            if not os.path.exists(p): continue
            s, sr = read_wav(p)
            t0 = time.perf_counter(); txt = decode(rec, s, sr); t_inf += time.perf_counter() - t0
            t_aud += len(s) / sr
            texts.append(txt)
            if wake_hit(txt, keys, use_norm, fuzzy): hit += 1
        out.append(dict(word=name, n=len(texts), hit=hit,
                        rate=round(hit / max(1, len(texts)) * 100, 1),
                        rtf=round(t_inf / max(1e-9, t_aud), 4), texts=texts))
    res = dict(model=os.path.basename(dirname.rstrip("\\/")), hot=(hotwords_file and os.path.basename(hotwords_file)) or "-",
               score=hotwords_score, mb=round(size_mb(dirname, model_file), 1), load_s=round(load_s, 2),
               peak_mb=round(peak_rss_mb()[0], 1), words=out)
    if json_out:
        with open(json_out, "w", encoding="utf-8") as f:
            json.dump(res, f, ensure_ascii=False, indent=1)
    return res


# ---------------- 流式（真实上板形态） ----------------
def build_stream(dirname, hotwords_file="", score=1.5, threads=2):
    d = os.path.join(T, dirname) if not os.path.isabs(dirname) else dirname
    f = lambda n: os.path.join(d, n)
    enc = "encoder-epoch-99-avg-1.int8.onnx"
    if not os.path.exists(f(enc)):
        enc = sorted(x for x in os.listdir(d) if x.startswith("encoder") and x.endswith(".onnx"))[0]
    dec = [x for x in os.listdir(d) if x.startswith("decoder") and x.endswith(".onnx")][0]
    joi = [x for x in os.listdir(d) if x.startswith("joiner") and x.endswith(".onnx")][0]
    t0 = time.perf_counter()
    rec = sherpa_onnx.OnlineRecognizer.from_transducer(
        tokens=f("tokens.txt"), encoder=f(enc), decoder=f(dec), joiner=f(joi),
        num_threads=threads, sample_rate=16000, feature_dim=80,
        decoding_method=("modified_beam_search" if hotwords_file else "greedy_search"),
        max_active_paths=4,
        hotwords_file=hotwords_file, hotwords_score=score,
        modeling_unit="cjkchar", enable_endpoint_detection=False)
    return rec, time.perf_counter() - t0

def decode_stream(rec, samples):
    st = rec.create_stream()
    st.accept_waveform(16000, samples)
    st.accept_waveform(16000, [0.0] * 8000)   # 补 0.5s 静音，逼出尾部 token
    st.input_finished()
    while rec.is_ready(st):
        rec.decode_stream(st)
    r = rec.get_result(st)
    return r if isinstance(r, str) else r.text

def run_wake_stream(dirname, hotwords_file="", score=1.5, json_out=None, use_norm=False, fuzzy=False):
    rec, load_s = build_stream(dirname, hotwords_file, score)
    out = []
    for name, (files, keys) in WAKE.items():
        hit, texts, t_inf, t_aud = 0, [], 0.0, 0.0
        for p in files:
            if not os.path.exists(p): continue
            s, sr = read_wav(p)
            t0 = time.perf_counter(); txt = decode_stream(rec, s); t_inf += time.perf_counter() - t0
            t_aud += len(s) / sr
            texts.append(txt)
            if wake_hit(txt, keys, use_norm, fuzzy): hit += 1
        out.append(dict(word=name, n=len(texts), hit=hit,
                        rate=round(hit / max(1, len(texts)) * 100, 1),
                        rtf=round(t_inf / max(1e-9, t_aud), 4), texts=texts))
    res = dict(model=os.path.basename(dirname.rstrip("\\/")) + "(stream)",
               norm=use_norm, fuzzy=fuzzy, hot=(hotwords_file and os.path.basename(hotwords_file)) or "-", score=score,
               mb=round(size_mb(dirname), 1), load_s=round(load_s, 2),
               peak_mb=round(peak_rss_mb()[0], 1), words=out)
    if json_out:
        json.dump(res, open(json_out, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    return res

def fmt_cer(rows_all):
    for r, rows in rows_all:
        print(f"[{r['tag']:>6}] {r['model']:<16} 体积{r['mb']:>6.1f}MB  加载{r['load_s']:>5.2f}s  "
              f"CER {r['cer']:>5.2f}%  全对 {r['exact']}/{r['n']}  RTF {r['rtf']:.4f}  峰值{r['peak_mb']:.0f}MB")
        bad = [x for x in rows if x[3] > 0]
        for w, ref, hyp, c in bad[:6]:
            print(f"        {w} 参考[{ref}] 识别[{hyp}] CER {c}")

def fmt_wake(res):
    print(f"{res['model']:<14} 热词={res['hot']:<12} 归一={res.get('norm')} 近音={res.get('fuzzy')} 体积{res['mb']:.1f}MB "
          f"峰值{res['peak_mb']:.0f}MB")
    for w in res["words"]:
        print(f"   {w['word']}: 命中 {w['hit']}/{w['n']} = {w['rate']}%")
        for t in w["texts"][:12]:
            print(f"      · {t}")

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["cer", "wake", "scan", "quant", "hot", "stwake"])
    ap.add_argument("dir", nargs="?", default="")
    ap.add_argument("--snr", default="", help="如 20,10,5；空=只跑干净")
    ap.add_argument("--json", default="")
    ap.add_argument("--hot", default="", help="hotwords 文件路径")
    ap.add_argument("--score", type=float, default=1.5)
    ap.add_argument("--file", default="", help="模型目录内的具体 onnx 文件名")
    ap.add_argument("--norm", action="store_true", help="开启同音字归一")
    ap.add_argument("--fuzzy", action="store_true", help="归一 + 近音容错（激进）")
    a = ap.parse_args()

    if a.cmd == "scan":
        for d in sorted(os.listdir(T)):
            p = os.path.join(T, d)
            if os.path.isdir(p) and any(x.endswith(".onnx") or x.endswith(".ort") for x in os.listdir(p)):
                print(f"{d:<26} {size_mb(d):>8.1f}MB")
    elif a.cmd == "cer":
        snrs = [None] + [float(x) for x in a.snr.split(",") if x.strip()] if a.snr else [None]
        fmt_cer(run_cer(a.dir, snrs, a.json or None, a.file))
    elif a.cmd == "wake":
        fmt_wake(run_wake(a.dir, a.hot, a.score, a.json or None, a.file, a.norm, a.fuzzy))
    elif a.cmd == "quant":
        cases = [("m_zipctc_fp32", "model.onnx", "fp32 原始"),
                 ("m_zipctc_fp32", "model_q.int8.onnx", "自量化 int8"),
                 ("m_zipctc_s", "model.int8.onnx", "官方 int8")]
        allr = []
        for d, f, tag in cases:
            if not os.path.exists(os.path.join(T, d, f)): continue
            rs = run_cer(d, [None], model_file=f)
            rs[0][0]["tag"] = tag
            allr.append(rs[0])
        fmt_cer(allr)
        if a.json:
            json.dump([{"sum": r, "rows": rows} for r, rows in allr], open(a.json, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    elif a.cmd == "hot":
        fmt_wake(run_wake(a.dir, a.hot, a.score, a.json or None, a.file, a.norm, a.fuzzy))
    elif a.cmd == "stwake":
        fmt_wake(run_wake_stream(a.dir, a.hot, a.score, a.json or None, a.norm, a.fuzzy))
