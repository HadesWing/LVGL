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


def list_devices(ffmpeg):
    out = subprocess.run([ffmpeg, "-hide_banner", "-list_devices", "true",
                          "-f", "dshow", "-i", "dummy"],
                         capture_output=True, text=True, errors="replace").stderr
    devs = re.findall(r'"([^"]+)" \(audio\)', out)
    for i, d in enumerate(devs):
        print("  [%d] %s" % (i, d))
    return devs


def run_mic(kws, ffmpeg, device):
    # -nostdin：ffmpeg 默认会抢 stdin，被后台跑时会挂在读 stdin 上
    # stderr 给 DEVNULL：进度日志不读的话，64KB 管道写满会把 ffmpeg 堵死
    cmd = [ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "quiet",
           "-f", "dshow", "-i", "audio=" + device,
           "-ar", "16000", "-ac", "1", "-f", "s16le", "-"]
    print("麦克风: %s" % device)
    print("开始监听…… 对着麦克风喊唤醒词（Ctrl+C 退出）\n")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=0,
                         stderr=subprocess.DEVNULL)
    fd = p.stdout.fileno()
    stream = kws.create_stream()
    t_start = time.time()
    nbytes = 0
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
                print("采集流结束（ffmpeg 退出，rc=%s，已收 %d 字节）"
                      % (p.poll(), nbytes))
                break
            if nbytes == 0:
                print("首块到手: %d 字节，等 %.2fs（此后每 100ms 一块）"
                      % (len(raw), time.time() - t_blk))
            pcm = array.array("h")
            pcm.frombytes(raw)
            nbytes += len(raw)
            t_audio = nbytes / 2.0 / 16000.0
            stream.accept_waveform(16000, pcm)
            while kws.is_ready(stream):
                kws.decode_stream(stream)
            r = kws.get_result(stream)
            if r:
                print("🔔 [%7.2fs 音频 / %7.1fs 墙钟] 检出: %s" %
                      (t_audio, time.time() - t_start, r))
                kws.reset_stream(stream)
    except KeyboardInterrupt:
        print("\n停止。共监听 %.1fs 音频（%.1fs 墙钟）" %
              (nbytes / 2.0 / 16000.0, time.time() - t_start))
    finally:
        p.kill()


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
    args = ap.parse_args()

    ffmpeg = shutil.which("ffmpeg")
    if args.list_dev:
        if not ffmpeg:
            die("PATH 里没有 ffmpeg")
        list_devices(ffmpeg)
        return 0

    if not args.model_dir:
        die("缺 --model-dir（或用环境变量 KWS_MODEL_DIR）")
    if not args.keywords:
        die("缺 --keywords")
    if not args.mic and not args.wav:
        die("要么给 wav 文件，要么用 --mic")

    kws = load_spotter(args.model_dir, args.keywords, args.threshold, args.score)
    print("模型: %s\n阈值 %.2f / 加分 %.2f / keywords: %s\n" %
          (args.model_dir, args.threshold, args.score, args.keywords))

    if args.mic:
        if not ffmpeg:
            die("PATH 里没有 ffmpeg（MSYS2: /c/msys64/mingw64/bin）")
        dev = args.device
        if not dev:
            devs = list_devices(ffmpeg)
            if not devs:
                die("找不到音频采集设备")
            dev = devs[0]
        run_mic(kws, ffmpeg, dev)
        return 0

    return 0 if run_wav(kws, args.wav) else 1


if __name__ == "__main__":
    sys.exit(main())
