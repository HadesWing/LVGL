#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""离线唤醒词检测验证工具（sherpa-onnx KWS 中文模型）

两种模式：
  1) 离线：喂 wav 文件，逐个报告是否检出、在第几秒检出，最后给统计
  2) 实时：ffmpeg 抓麦克风 → 边喂边检，模拟真机常驻监听（Ctrl+C 退出）

用法：
  # 离线（可一次给多个 wav，支持通配符由 shell 展开）
  python scripts/kws_test.py --model-dir <dir> --keywords kw.txt a.wav b.wav

  # 实时麦克风（自动挑默认设备，或 --device 指定）
  python scripts/kws_test.py --model-dir <dir> --keywords kw.txt --mic

参数：
  --threshold  判决阈值（默认 0.25；调低更容易触发、误唤醒变多）
  --score      关键词加分（默认 1.0）
  --list-dev   列出 DirectShow 音频设备后退出

模型目录需要这三个 onnx + tokens.txt（sherpa-onnx KWS 中文 wenetspeech 3.3M）：
  encoder-epoch-99-avg-1-chunk-16-left-64.onnx
  decoder-epoch-99-avg-1-chunk-16-left-64.onnx
  joiner-epoch-99-avg-1-chunk-16-left-64.onnx
  tokens.txt

keywords 文件每行：拼音 token 用空格分隔 + @中文标签，例如
  n ǐ h ǎo x ī x ī @你好兮兮
"""
import argparse
import array
import math
import os
import re
import shutil
import subprocess
import sys
import time
import wave

ENC = "encoder-epoch-99-avg-1-chunk-16-left-64.onnx"
DEC = "decoder-epoch-99-avg-1-chunk-16-left-64.onnx"
JOI = "joiner-epoch-99-avg-1-chunk-16-left-64.onnx"
CHUNK = 1600          # 每块 1600 采样 = 100ms @ 16k，模拟流式喂入


def die(msg):
    print("错误: " + msg, file=sys.stderr)
    sys.exit(1)


def load_spotter(md, kwfile, thr, score):
    try:
        import sherpa_onnx
    except ImportError:
        die(
            "当前解释器没装 sherpa-onnx\n"
            "  解释器: %s\n"
            "  原因通常是机器上有多个 python，包装在了别的解释器里。\n"
            "  两个办法：\n"
            "    1) 用启动器: scripts/kws.sh --mic   （自动挑对解释器）\n"
            "    2) 给这个解释器装上: %s -m pip install sherpa-onnx sherpa-onnx-core"
            % (sys.executable, sys.executable)
        )
    for f in (ENC, DEC, JOI, "tokens.txt"):
        p = os.path.join(md, f)
        if not os.path.isfile(p):
            die("模型文件缺失: " + p)
    if not os.path.isfile(kwfile):
        die("keywords 文件不存在: " + kwfile)
    return sherpa_onnx.KeywordSpotter(
        tokens=os.path.join(md, "tokens.txt"),
        encoder=os.path.join(md, ENC),
        decoder=os.path.join(md, DEC),
        joiner=os.path.join(md, JOI),
        keywords_file=kwfile,
        num_threads=2,
        sample_rate=16000,
        feature_dim=80,
        max_active_paths=4,
        keywords_score=score,
        keywords_threshold=thr,
        num_trailing_blanks=1,
        provider="cpu",
    )


def read_wav_mono16k(path):
    """读 wav → (array('h'), rate)。只接受 16k 单声道 16bit，别的直接报错。"""
    with wave.open(path, "rb") as w:
        ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        data = w.readframes(n)
    if sw != 2:
        die("%s: 只支持 16bit，实际 %dbit" % (path, sw * 8))
    if ch != 1:
        die("%s: 只支持单声道，实际 %d ch" % (path, ch))
    if rate != 16000:
        die("%s: 只支持 16kHz，实际 %d（sv56 重采样后再来）" % (path, rate))
    pcm = array.array("h")
    pcm.frombytes(data)
    return pcm, rate


def feed_stream(kws, stream, pcm, rate, label=""):
    """分块喂 + 逐块解码。返回 [(检出词, 秒)]。"""
    hits = []
    total = len(pcm)
    for off in range(0, total, CHUNK):
        stream.accept_waveform(rate, pcm[off:off + CHUNK])
        while kws.is_ready(stream):
            kws.decode_stream(stream)
        r = kws.get_result(stream)
        if r:
            hits.append((r, (off + len(pcm[off:off + CHUNK])) / float(rate)))
            kws.reset_stream(stream)
            if label:
                print("  ✅ %-14s 第 %5.2fs 检出" % (r, hits[-1][1]))
    return hits


def run_wav(kws, paths):
    ok = miss = err = 0
    t0 = time.time()
    for p in paths:
        name = os.path.basename(p)
        try:
            pcm, rate = read_wav_mono16k(p)
        except Exception as e:                      # noqa: BLE001
            print("  ❌ %-22s 读取失败: %s" % (name, e))
            err += 1
            continue
        stream = kws.create_stream()
        hits = feed_stream(kws, stream, pcm, rate, label=name)
        dur = len(pcm) / float(rate)
        if hits:
            ok += 1
            print("  ── %-22s %.2fs 音频，检出: %s" %
                  (name, dur, ", ".join("%s@%.2fs" % (w, t) for w, t in hits)))
        else:
            miss += 1
            print("  ⚠️  %-22s %.2fs 音频，未检出" % (name, dur))
    cost = time.time() - t0
    n = len(paths)
    print("\n统计: 检出 %d / %d (%.0f%%)  未检出 %d  读取失败 %d  耗时 %.1fs (RTF %.3f)" %
          (ok, n, 100.0 * ok / n if n else 0, miss, err, cost, cost / max(n, 1)))
    return miss == 0 and err == 0


def enum_raw(ffmpeg):
    """跑一遍 dshow 设备枚举，返回 ffmpeg 的 stderr 原文。"""
    return subprocess.run([ffmpeg, "-hide_banner", "-list_devices", "true",
                           "-f", "dshow", "-i", "dummy"],
                          capture_output=True, text=True, errors="replace",
                          encoding="utf-8").stderr or ""


def parse_devices(out):
    """从枚举输出里解析音频设备名。

    ffmpeg 各版本格式不一样，必须同时兼容三种：
      <= 8.x  有段标题：
              '[dshow @ ...] DirectShow audio devices'
              '[dshow @ ...]  "麦克风 (Jabra)"'
      >= 9.0  没有段标题，改成行尾标注（9.0.2 实测）：
              '[in#0 @ ...] "麦克风 (Jabra EVOLVE 30 II)" (audio)'
              '[in#0 @ ...] Could not enumerate video devices (or none found).'
    因此判定优先级：行尾 (audio)/(video) 标注 > 当前所在段标题。
    """
    devs = []
    section = ""
    for line in out.splitlines():
        body = line.split("] ", 1)[-1].strip()
        low = body.lower()
        if "directshow video devices" in low:
            section = "video"
            continue
        if "directshow audio devices" in low:
            section = "audio"
            continue
        if not body.startswith('"') or body.count('"') < 2:
            continue      # "Alternative name ..." / "Could not enumerate ..." 在这里被过滤
        if body.endswith("(audio)"):
            kind = "audio"
        elif body.endswith("(video)"):
            kind = "video"
        else:
            kind = section      # 旧版靠段标题
        if kind != "audio":
            continue
        name = body[1:body.rindex('"')]
        if name and name not in devs:
            devs.append(name)
    return devs


def list_devices(ffmpeg, dump=False):
    out = enum_raw(ffmpeg)
    if dump:
        print("  --- ffmpeg 枚举原始输出 ---")
        for ln in out.splitlines():
            print("  | " + ln)
        print("  --- 原始输出结束 ---")
    devs = parse_devices(out)
    for i, d in enumerate(devs):
        print("  [%d] %s" % (i, d))
    return devs


def run_sweep(md, kwfile, paths, thrs, score):
    """同一批音频用不同阈值各跑一遍。

    这是排查"喊了不触发"的关键实验：分数只差一点（比如 0.18 对 0.25）时调阈值就好；
    分数差得远说明发音/音素对不上，再怎么调阈值也是误报换命中。
    """
    print("阈值扫描（同一批音频，只改 keywords_threshold）\n")
    head = "  %-24s" % "音频"
    for t in thrs:
        head += " %6.2f" % t
    print(head)

    spotters = {}
    for t in thrs:
        spotters[t] = load_spotter(md, kwfile, t, score)

    for p in paths:
        name = os.path.basename(p)
        try:
            pcm, rate = read_wav_mono16k(p)
        except Exception as e:                      # noqa: BLE001
            print("  ❌ %-22s %s" % (name, e))
            continue
        line = "  %-24s" % name[:24]
        for t in thrs:
            stream = spotters[t].create_stream()
            hits = feed_stream(spotters[t], stream, pcm, rate)
            line += " %6s" % ("命中" if hits else "—")
        print(line)

    print("\n读法：")
    print("  阈值降到 0.05~0.15 才命中 → 分数在边缘，先调低阈值；仍不稳就换近场话筒/换大模型")
    print("  0.05 也不命中          → 音素对不上，这个唤醒词的发音这个模型学不好，换词或换模型")


def run_grid(md, kwfile, paths, thrs, scores):
    """阈值 × 加分 二维扫描。

    排查"有声音但检不出"的关键实验：阈值决定放行门槛，加分决定解码时给关键词
    路径额外多少 boost。两个都要试，只看阈值容易误判成"音素对不上"。
    """
    print("阈值 × 加分 网格扫描（同一段音频，只改解码参数）\n")
    spotters = {}
    for t in thrs:
        for s in scores:
            spotters[(t, s)] = load_spotter(md, kwfile, t, s)

    for p in paths:
        try:
            pcm, rate = read_wav_mono16k(p)
        except Exception as e:                     # noqa: BLE001
            print("  ❌ %s: %s" % (os.path.basename(p), e))
            continue
        print("音频 %s  时长 %.2fs" % (os.path.basename(p), len(pcm) / float(rate)))
        head = "    阈值\\加分"
        for s in scores:
            head += " %8s" % ("%.1f" % s)
        print(head)
        best = None
        for t in thrs:
            line = "  %10.2f" % t
            for s in scores:
                stream = spotters[(t, s)].create_stream()
                hits = feed_stream(spotters[(t, s)], stream, pcm, rate)
                line += " %8s" % ("—" if not hits else "%d次" % len(hits))
                if hits:
                    # 命中数优先（稳），其次阈值高（少误报），再其次加分低（自然）
                    key = (len(hits), t, -s)
                    if best is None or key > best[0]:
                        best = (key, t, s, len(hits))
            print(line)
        if best:
            _, bt, bs, bn = best
            print("  ➜ 推荐: --threshold %.2f --score %.1f （该格命中 %d 次，"
                  "是表中命中最多且阈值最高的组合）" % (bt, bs, bn))
        else:
            print("  ➜ 全表无命中：这段音频的发音/音素与模型对不上，调阈值和加分都救不回来，"
                  "换唤醒词或换更大的 KWS 模型。")
        print("")
    print("读法：")
    print("  有命中的格子里次数越多越稳。优先选「阈值尽量高 + 加分尽量低」还能命中的那格，")
    print("  这样误报最少；全表都是 — 说明这段音频的发音/音素确实对不上，换词或换模型。")


def split_speech(pcm, rate=16000, frame_ms=50, rel_thr=0.06,
                 min_gap_ms=250, min_seg_ms=300, pad_ms=150, tail_ms=500):
    """按能量把长录音切成一句一句，返回 [(起采样, 止采样)]。

    用途：一段 40 秒录音里到底说了几句、哪几句没检出，比只看总命中数有用得多。
    阈值取「峰值帧能量 × rel_thr」和绝对下限 60 的较大者，避免静音段被切进来。
    句尾额外补 tail_ms 静音：KWS 要攒够 num_trailing_blanks 才会输出结果，
    尾部不给静音会被判成"没说完"，逐句测就会出现假漏检。
    """
    n = max(1, int(rate * frame_ms / 1000))
    frames = []                     # [(起采样, rms)]
    for i in range(0, max(1, len(pcm) - n + 1), n):
        blk = pcm[i:i + n]
        e = 0
        for v in blk:
            e += v * v
        frames.append((i, math.sqrt(e / float(len(blk)))))
    if not frames:
        return []
    thr = max(60.0, max(r for _, r in frames) * rel_thr)
    voiced = [(i, r > thr) for i, r in frames]

    segs = []
    start = None
    last_on = None
    gap_frames = max(1, int(min_gap_ms / frame_ms))
    for idx, (i, on) in enumerate(voiced):
        if on:
            if start is None:
                start = i
            last_on = idx
        elif start is not None and idx - last_on >= gap_frames:
            segs.append((start, voiced[last_on][0] + n))
            start = None
    if start is not None and last_on is not None:
        segs.append((start, voiced[last_on][0] + n))

    pad = int(rate * pad_ms / 1000)
    tail = int(rate * tail_ms / 1000)
    min_len = int(rate * min_seg_ms / 1000)
    out = []
    for s, e in segs:
        if e - s < min_len:
            continue
        out.append((max(0, s - pad), min(len(pcm), e + tail)))
    return out


def clip_ratio(pcm):
    """满量程样本占比（%）。削顶会让 fbank 特征失真，是"有声音但检不出"的常见元凶。"""
    if not pcm:
        return 0.0
    n = 0
    for v in pcm:
        if v >= 32700 or v <= -32700:
            n += 1
    return 100.0 * n / len(pcm)


def run_segments(md, kwfile, path, thr, score):
    """逐句诊断：把长录音切句，逐句跑检测，看漏在哪、漏的那些电平如何。"""
    pcm, rate = read_wav_mono16k(path)
    segs = split_speech(pcm, rate)
    print("音频 %s  时长 %.2fs  切出 %d 句（阈值 %.2f / 加分 %.1f）\n"
          % (os.path.basename(path), len(pcm) / float(rate), len(segs), thr, score))
    if not segs:
        print("  没切出语音段，可能全程静音或底噪判定异常")
        return
    kws = load_spotter(md, kwfile, thr, score)
    print("   #   起(s)  长(s)    rms   peak  削顶%  结果")
    hit = 0
    for i, (s, e) in enumerate(segs, 1):
        part = pcm[s:e]
        rms, peak, _ = level_meter(part)
        stream = kws.create_stream()
        hits = feed_stream(kws, stream, part, rate)
        if hits:
            hit += 1
        print("  %2d  %6.2f %6.2f %6d %6d %5.2f   %s"
              % (i, s / float(rate), len(part) / float(rate), rms, peak,
                 clip_ratio(part),
                 ",".join(w for w, _ in hits) if hits else "—"))
    print("\n  检出 %d / %d 句 (%.0f%%)" % (hit, len(segs), 100.0 * hit / len(segs)))
    print("  漏检句电平明显偏低 → 先解决麦克风增益/距离；")
    print("  削顶% 高的句子漏检 → 输入过载，把系统麦克风音量降到 70% 左右再录；")
    print("  电平充足、无削顶仍漏检 → 模型对这个发音不敏感，调参救不回来。")


def level_meter(pcm):
    """返回 (rms, peak, 20 格音量条)。dBFS 映射到 -60..0 dB，一眼看出麦克风在工作没。"""
    n = len(pcm)
    if n == 0:
        return 0, 0, "·" * 20
    peak = max(max(pcm), -min(pcm))
    energy = 0
    for v in pcm:
        energy += v * v
    rms = int(math.sqrt(energy / float(n)))
    if rms <= 0:
        return 0, peak, "·" * 20
    db = 20.0 * math.log10(rms / 32768.0)
    k = 0 if db < -60 else min(20, int((db + 60) / 3.0))
    return rms, peak, "█" * k + "·" * (20 - k)


def run_mic(kws, ffmpeg, device, save_wav="", meter=True):
    # -nostdin：ffmpeg 默认会抢 stdin，被后台跑时会挂在读 stdin 上
    # stderr 给 DEVNULL：进度日志不读的话，64KB 管道写满会把 ffmpeg 堵死
    cmd = [ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "quiet",
           "-f", "dshow", "-i", "audio=" + device,
           "-ar", "16000", "-ac", "1", "-f", "s16le", "-"]
    print("麦克风: %s" % device)
    print("开始监听…… 对着麦克风喊唤醒词（Ctrl+C 退出）")
    if save_wav:
        print("同时把采集音频写进 %s（事后可离线复测）" % save_wav)
    print("")

    wf = None
    if save_wav:
        wf = wave.open(save_wav, "wb")
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(16000)

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=0,
                         stderr=subprocess.DEVNULL)
    fd = p.stdout.fileno()
    stream = kws.create_stream()
    t_start = time.time()
    nbytes = 0
    nblk = 0
    rms_max = 0
    loud_blk = 0          # rms>200（约 -44dBFS）的块数，近似"这段有人在说话"
    hits = 0
    try:
        while True:
            # 用 os.read 直读 fd：Python 的 BufferedReader 在 Windows pipe 上
            # 会短读甚至误报 EOF，而 os.read 只管"有多少给多少"。
            t_blk = time.time()
            raw = b""
            while len(raw) < CHUNK * 2:
                piece = os.read(fd, CHUNK * 2 - len(raw))
                if not piece:
                    break
                raw += piece
            if len(raw) < CHUNK * 2:
                print("\n采集流结束（ffmpeg 退出，rc=%s，已收 %d 字节）"
                      % (p.poll(), nbytes))
                break
            if nbytes == 0:
                print("首块到手: %d 字节，等 %.2fs（此后每 100ms 一块）"
                      % (len(raw), time.time() - t_blk))
            pcm = array.array("h")
            pcm.frombytes(raw)
            nbytes += len(raw)
            if wf is not None:
                wf.writeframes(raw)
            t_audio = nbytes / 2.0 / 16000.0

            rms, peak, bar = level_meter(pcm)
            nblk += 1
            rms_max = max(rms_max, rms)
            if rms > 200:
                loud_blk += 1
            if meter and nblk % 2 == 0:
                sys.stdout.write("\r[%6.2fs] %s rms=%5d peak=%5d " %
                                 (t_audio, bar, rms, peak))
                sys.stdout.flush()

            stream.accept_waveform(16000, pcm)
            while kws.is_ready(stream):
                kws.decode_stream(stream)
            r = kws.get_result(stream)
            if r:
                hits += 1
                if meter:
                    sys.stdout.write("\n")
                print("🔔 [%7.2fs 音频 / %7.1fs 墙钟] 检出: %s" %
                      (t_audio, time.time() - t_start, r))
                kws.reset_stream(stream)
    except KeyboardInterrupt:
        print("\n停止。共监听 %.1fs 音频（%.1fs 墙钟）" %
              (nbytes / 2.0 / 16000.0, time.time() - t_start))
    finally:
        p.kill()
        if wf is not None:
            wf.close()
            print("已保存采集音频: %s（%.1fs）" %
                  (save_wav, nbytes / 2.0 / 16000.0))

    if nblk:
        print("电平统计: rms 峰值 %d，有声块 %d/%d (%.0f%%)，检出 %d 次" %
              (rms_max, loud_blk, nblk, 100.0 * loud_blk / nblk, hits))
        if rms_max < 100:
            print("  ⚠️ 全程几乎没电平：麦克风没收到声音（设备选错 / 被静音 / 增益为 0）")
        elif loud_blk * 20 < nblk:
            print("  ⚠️ 有声块很少：喊话离麦太远或增益太低，KWS 拿到的信噪比不够")
    return hits


def find_ffmpeg(hint=""):
    """定位 ffmpeg，不依赖 conda activate / PATH 是否被初始化。

    顺序：命令行 --ffmpeg > 环境变量 KWS_FFMPEG > 与当前解释器同环境的 ffmpeg
    （conda 环境装在 <env>/Library/bin）> PATH > 几个已知绝对路径。
    返回 (路径, 来源说明)，找不到返回 (None, "")。
    """
    cands = []
    if hint:
        cands.append((hint, "--ffmpeg"))
    env = os.environ.get("KWS_FFMPEG", "")
    if env:
        cands.append((env, "KWS_FFMPEG"))
    exe_dir = os.path.dirname(os.path.abspath(sys.executable))
    # conda 环境：python.exe 在 <env>\python.exe，ffmpeg 在 <env>\Library\bin
    # 类 unix 布局：<env>/bin/python 同目录就是 bin
    for sub in ("Library/bin", "bin", "Scripts",
                os.path.join("..", "Library", "bin")):
        cands.append((os.path.join(exe_dir, sub, "ffmpeg.exe"),
                      "解释器同环境"))
    w = shutil.which("ffmpeg")
    if w:
        cands.append((w, "PATH"))
    for p in (r"C:\msys64\mingw64\bin\ffmpeg.exe",
              r"C:\msys64\usr\bin\ffmpeg.exe",
              r"D:\Program Files\anaconda3\envs\kws\Library\bin\ffmpeg.exe"):
        cands.append((p, "已知路径"))
    for path, src in cands:
        if path and os.path.isfile(path):
            return os.path.abspath(path), src
    return None, ""


def ffmpeg_version(ffmpeg):
    try:
        out = subprocess.run([ffmpeg, "-hide_banner", "-version"],
                             capture_output=True, text=True, errors="replace",
                             encoding="utf-8").stdout or ""
        first = out.splitlines()[0] if out else ""
        m = re.search(r"version\s+(\S+)", first)
        return m.group(1) if m else first.strip()
    except Exception:                                  # noqa: BLE001
        return "未知"


def main():
    ap = argparse.ArgumentParser(description="离线唤醒词检测验证（sherpa-onnx KWS）")
    ap.add_argument("wav", nargs="*", help="待测 wav（16k 单声道 16bit）")
    ap.add_argument("--model-dir", default=os.environ.get("KWS_MODEL_DIR", ""))
    ap.add_argument("--keywords", default="", help="keywords 文件（拼音 + @中文）")
    ap.add_argument("--threshold", type=float, default=0.25)
    ap.add_argument("--score", type=float, default=1.0)
    ap.add_argument("--mic", action="store_true", help="实时麦克风模式")
    ap.add_argument("--device", default="", help="DirectShow 设备名（默认第一个音频设备）")
    ap.add_argument("--list-dev", action="store_true", help="列出音频设备")
    ap.add_argument("--ffmpeg", default="", metavar="PATH",
                    help="指定 ffmpeg 可执行文件（也可用 KWS_FFMPEG 环境变量）")
    ap.add_argument("--save-wav", default="", metavar="PATH",
                    help="--mic 时把采集到的 16k 单声道音频另存为 wav，便于离线复测")
    ap.add_argument("--no-meter", action="store_true", help="关闭实时电平条")
    ap.add_argument("--sweep", action="store_true",
                    help="阈值扫描：同一批 wav 用多个阈值各跑一遍，判断分数差多少")
    ap.add_argument("--sweep-thrs", default="0.05,0.10,0.15,0.20,0.25",
                    help="扫描用的阈值列表，逗号分隔")
    ap.add_argument("--grid", action="store_true",
                    help="阈值 × 加分 二维网格扫描（排查'有声音但检不出'最有效）")
    ap.add_argument("--grid-thrs", default="0.05,0.10,0.15,0.20,0.25,0.30")
    ap.add_argument("--grid-scores", default="1.0,1.5,2.0,3.0")
    ap.add_argument("--autotune", action="store_true",
                    help="--mic 录完后自动对这段录音跑一次网格，直接给出该用哪组参数")
    ap.add_argument("--segments", action="store_true",
                    help="逐句诊断：把长录音按能量切句，逐句跑检测，看漏在哪几句")
    args = ap.parse_args()

    ffmpeg, ff_src = find_ffmpeg(args.ffmpeg)
    if args.list_dev or args.mic:
        if not ffmpeg:
            print("错误: 找不到 ffmpeg。试过：--ffmpeg 参数、KWS_FFMPEG、"
                  "解释器同环境、PATH、C:\\msys64\\mingw64\\bin", file=sys.stderr)
            print("      MSYS2 安装：pacman -S mingw-w64-x86_64-ffmpeg", file=sys.stderr)
            print("      conda 安装：conda install -n kws -c conda-forge ffmpeg",
                  file=sys.stderr)
            return 1
        print("[ffmpeg] %s（来源: %s，版本 %s）"
              % (ffmpeg, ff_src, ffmpeg_version(ffmpeg)))

    if args.list_dev:
        devs = list_devices(ffmpeg, dump=not parse_devices(enum_raw(ffmpeg)))
        if not devs:
            print("\n没有解析到音频设备，上面是 ffmpeg 的原始输出。", file=sys.stderr)
            print("若原始输出里能看到你的麦克风，说明是解析器不认这个版本的格式，"
                  "把原始输出发出来即可；", file=sys.stderr)
            print("若原始输出里也没有，用 --device \"设备名\" 手动指定，"
                  "先确认 Windows 隐私设置允许桌面应用访问麦克风。", file=sys.stderr)
            return 1
        return 0

    if not args.model_dir:
        die("缺 --model-dir（或用环境变量 KWS_MODEL_DIR）")
    if not args.keywords:
        die("缺 --keywords")
    if not args.mic and not args.wav:
        die("要么给 wav 文件，要么用 --mic")

    if args.sweep:
        if not args.wav:
            die("--sweep 需要给 wav 文件（先用 --mic --save-wav 录一段）")
        thrs = [float(x) for x in args.sweep_thrs.split(",") if x.strip()]
        run_sweep(args.model_dir, args.keywords, args.wav, thrs, args.score)
        return 0

    if args.grid:
        if not args.wav:
            die("--grid 需要给 wav 文件（先用 --mic --save-wav 录一段）")
        thrs = [float(x) for x in args.grid_thrs.split(",") if x.strip()]
        scores = [float(x) for x in args.grid_scores.split(",") if x.strip()]
        run_grid(args.model_dir, args.keywords, args.wav, thrs, scores)
        return 0

    if args.segments:
        if not args.wav:
            die("--segments 需要给 wav 文件（先用 --mic --save-wav 录一段）")
        for p in args.wav:
            run_segments(args.model_dir, args.keywords, p,
                         args.threshold, args.score)
        return 0

    kws = load_spotter(args.model_dir, args.keywords, args.threshold, args.score)
    print("模型: %s\n阈值 %.2f / 加分 %.2f / keywords: %s\n" %
          (args.model_dir, args.threshold, args.score, args.keywords))

    if args.mic:
        dev = args.device
        if not dev:
            devs = list_devices(ffmpeg, dump=True)
            if not devs:
                print("\n错误: 找不到音频采集设备（上面是 ffmpeg 原始输出）。",
                      file=sys.stderr)
                print("可用 --device \"设备名\" 手动指定；也要确认 Windows "
                      "设置 > 隐私 > 麦克风 允许桌面应用访问。", file=sys.stderr)
                return 1
            dev = devs[0]
        hits = run_mic(kws, ffmpeg, dev, save_wav=args.save_wav,
                       meter=not args.no_meter)
        if not hits:
            del kws                       # 先释放，网格要再建十几个 spotter
            if args.save_wav and os.path.isfile(args.save_wav):
                if args.autotune:
                    print("\n本次未检出，自动对刚才这段录音做参数网格扫描……\n")
                    thrs = [float(x) for x in args.grid_thrs.split(",") if x.strip()]
                    scores = [float(x) for x in args.grid_scores.split(",") if x.strip()]
                    run_grid(args.model_dir, args.keywords, [args.save_wav],
                             thrs, scores)
                else:
                    print("\n本次 0 检出。录音已保存，加 --autotune 重跑即可自动调参，"
                          "或手动：--grid \"%s\"" % args.save_wav)
            else:
                print("\n本次 0 检出，且没保存录音。建议加 --save-wav <路径> --autotune "
                      "再录一次：\n"
                      "  录音落盘后可以对同一段音频反复试参数，不必反复喊。")
        return 0 if hits else 2

    return 0 if run_wav(kws, args.wav) else 1


if __name__ == "__main__":
    sys.exit(main())
