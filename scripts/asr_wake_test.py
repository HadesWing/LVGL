#!/usr/bin/env python3
"""用「流式 ASR + 文本匹配」替代 KWS 做唤醒词检测。

背景：sherpa-onnx 的 KWS 只有 3.3M 一个中文模型（kws-zipformer-wenetspeech-3.3M），
实测对部分说话人召回很低（同一段 40s 录音说 12 次只中 1 次，且任何阈值/加分组合
都救不回来）。流式 ASR 模型大得多、泛化好，把识别结果做文本匹配反而更稳。

用法（工作目录 lvgl_ws/lv_port_pc_vscode）：
  bash scripts/asr.sh --wav ../../.tools/kws/mic_xixi.wav            # 连续识别
  bash scripts/asr.sh --wav ../../.tools/kws/mic_xixi.wav --segments # 逐句 + 召回率
  bash scripts/asr.sh --mic                                          # 实时麦克风

模型目录默认 ../../../.tools/asr（sherpa-onnx-streaming-zipformer-zh-14M-2023-02-23）。
"""

import argparse
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import kws_test as K          # noqa: E402  复用读 wav / 切句 / 电平 / ffmpeg 定位

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MD = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".tools", "asr"))
ENC = "encoder-epoch-99-avg-1.int8.onnx"
DEC = "decoder-epoch-99-avg-1.int8.onnx"
JOI = "joiner-epoch-99-avg-1.int8.onnx"

# 同音/近音变体：ASR 常把「兮兮」听成「西西/嘻嘻/细细/兮兮」等
DEFAULT_WAKE = "你好兮兮,你好西西,你好嘻嘻,你好细细,你好熙熙"


def build_recognizer(md):
    import sherpa_onnx
    for f in (ENC, DEC, JOI, "tokens.txt"):
        p = os.path.join(md, f)
        if not os.path.isfile(p):
            K.die("模型文件缺失: " + p)
    return sherpa_onnx.OnlineRecognizer.from_transducer(
        tokens=os.path.join(md, "tokens.txt"),
        encoder=os.path.join(md, ENC),
        decoder=os.path.join(md, DEC),
        joiner=os.path.join(md, JOI),
        num_threads=2,
        sample_rate=16000,
        feature_dim=80,
        decoding_method="greedy_search",
        provider="cpu",
        enable_endpoint_detection=True,
        rule1_min_trailing_silence=2.4,
        rule2_min_trailing_silence=0.8,
        rule3_min_utterance_length=20.0,
    )


def edit_distance(a, b, cap=99):
    """Levenshtein，超过 cap 就提前返回（只为模糊匹配，不追求精确值）。"""
    if abs(len(a) - len(b)) > cap:
        return cap + 1
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def find_wake(text, wake_words, tol_ratio=0.25):
    """在识别文本里找唤醒词，允许少量错字（ASR 常见）。返回命中的词或 None。"""
    if not text:
        return None
    for w in wake_words:
        tol = max(1, int(len(w) * tol_ratio))
        if w in text:
            return w
        for i in range(0, max(1, len(text) - len(w) + 2)):
            piece = text[i:i + len(w)]
            if len(piece) < len(w):
                continue
            if edit_distance(piece, w, cap=tol) <= tol:
                return w
    return None


def decode_stream(rec, stream, pcm, rate, chunk=K.CHUNK):
    """把 pcm 分块喂完并解码，返回最终文本。"""
    for off in range(0, len(pcm), chunk):
        stream.accept_waveform(rate, pcm[off:off + chunk])
        while rec.is_ready(stream):
            rec.decode_stream(stream)
    stream.input_finished()
    while rec.is_ready(stream):
        rec.decode_stream(stream)
    return rec.get_result(stream)


def run_wav(rec, path, wake_words):
    pcm, rate = K.read_wav_mono16k(path)
    print("音频 %s  时长 %.2fs\n" % (os.path.basename(path), len(pcm) / float(rate)))

    # 连续识别：边喂边打印，看唤醒词在第几秒出现
    stream = rec.create_stream()
    chunk = K.CHUNK
    last = ""
    hits = []
    for off in range(0, len(pcm), chunk):
        stream.accept_waveform(rate, pcm[off:off + chunk])
        while rec.is_ready(stream):
            rec.decode_stream(stream)
        text = rec.get_result(stream)
        if text != last:
            last = text
            hit = find_wake(text, wake_words)
            mark = "  <<< 命中" if hit else ""
            print("  [%5.2fs] %s%s" % (min(off + chunk, len(pcm)) / float(rate),
                                       text, mark))
            if hit:
                hits.append((min(off + chunk, len(pcm)) / float(rate), hit))
        if rec.is_endpoint(stream):
            rec.reset(stream)
    print("\n连续识别命中 %d 次" % len(hits))
    for t, w in hits:
        print("  %.2fs  %s" % (t, w))
    return hits


def run_segments(rec, path, wake_words):
    pcm, rate = K.read_wav_mono16k(path)
    segs = K.split_speech(pcm, rate)
    print("音频 %s  时长 %.2fs  切出 %d 句\n"
          % (os.path.basename(path), len(pcm) / float(rate), len(segs)))
    if not segs:
        print("  没切出语音段")
        return []
    print("   #   起(s)  长(s)    rms   识别文本 / 命中")
    hits = []
    for i, (s, e) in enumerate(segs, 1):
        part = pcm[s:e]
        rms, _, _ = K.level_meter(part)
        stream = rec.create_stream()
        text = decode_stream(rec, stream, part, rate)
        hit = find_wake(text, wake_words)
        if hit:
            hits.append((i, text, hit))
        print("  %2d  %6.2f %6.2f %6d   %s%s"
              % (i, s / float(rate), len(part) / float(rate), rms,
                 text if text else "(空)",
                 "" if not hit else "   <<< " + hit))
    print("\n召回 %d / %d 句 (%.0f%%)"
          % (len(hits), len(segs), 100.0 * len(hits) / len(segs)))
    return hits


def run_mic(rec, wake_words):
    ffmpeg, src = K.find_ffmpeg("")
    if not ffmpeg:
        K.die("找不到 ffmpeg")
    ffver = K.ffmpeg_version(ffmpeg)
    print("[ffmpeg] %s（来源: %s，版本 %s）" % (ffmpeg, src, ffver))
    devs = K.list_devices(ffmpeg)
    if not devs:
        K.die("找不到音频采集设备")
    dev = devs[0]
    print("麦克风: %s\n开始监听…… 喊唤醒词（Ctrl+C 退出）\n" % dev)

    import subprocess
    cmd = [ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "quiet",
           "-f", "dshow", "-i", "audio=" + dev,
           "-ar", "16000", "-ac", "1", "-f", "s16le", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=0,
                         stderr=subprocess.DEVNULL)
    fd = p.stdout.fileno()
    stream = rec.create_stream()
    import array
    import time
    t0 = time.time()
    nbytes = 0
    hits = []
    try:
        while True:
            raw = b""
            while len(raw) < K.CHUNK * 2:
                piece = os.read(fd, K.CHUNK * 2 - len(raw))
                if not piece:
                    break
                raw += piece
            if len(raw) < K.CHUNK * 2:
                print("采集流结束（rc=%s）" % p.poll())
                break
            pcm = array.array("h")
            pcm.frombytes(raw)
            nbytes += len(raw)
            stream.accept_waveform(16000, pcm)
            while rec.is_ready(stream):
                rec.decode_stream(stream)
            text = rec.get_result(stream)
            hit = find_wake(text, wake_words)
            if hit and (not hits or text != hits[-1][1]):
                hits.append((time.time() - t0, text))
                print("🔔 [%6.2fs] %s" % (time.time() - t0, text))
            if rec.is_endpoint(stream):
                if text:
                    print("   [%6.2fs] 句: %s" % (time.time() - t0, text))
                rec.reset(stream)
    except KeyboardInterrupt:
        pass
    finally:
        p.kill()
    print("\n共监听 %.1fs，命中 %d 次" % (nbytes / 2.0 / 16000.0, len(hits)))
    return hits


def main():
    ap = argparse.ArgumentParser(description="流式 ASR 唤醒词检测（替代 KWS）")
    ap.add_argument("--model-dir", default=DEFAULT_MD)
    ap.add_argument("--wake", default=DEFAULT_WAKE, help="唤醒词变体，逗号分隔")
    ap.add_argument("--wav", default="", help="待测 wav（16k 单声道）")
    ap.add_argument("--segments", action="store_true", help="按能量切句逐句识别")
    ap.add_argument("--mic", action="store_true", help="实时麦克风")
    args = ap.parse_args()

    wake_words = [w.strip() for w in args.wake.split(",") if w.strip()]
    rec = build_recognizer(args.model_dir)
    print("模型: %s（中文流式 zipformer 14M）\n唤醒词: %s\n"
          % (args.model_dir, " / ".join(wake_words)))

    if args.mic:
        run_mic(rec, wake_words)
        return 0
    if not args.wav:
        K.die("给 --wav 或 --mic")
    if args.segments:
        run_segments(rec, args.wav, wake_words)
    else:
        run_wav(rec, args.wav, wake_words)
    return 0


if __name__ == "__main__":
    sys.exit(main())
