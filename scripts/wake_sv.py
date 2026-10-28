#!/usr/bin/env python3
"""离线唤醒（SenseVoice 版）：VAD 切句 + SenseVoice 识别 + 唤醒词模糊匹配。

为什么不用 KWS：sherpa-onnx 只有一个中文 KWS 模型（kws-zipformer-wenetspeech-3.3M），
实测对真实说话人召回极低——同一段 40s 录音说 12 次「你好兮兮」，KWS 只中 1 次；
拆开验证发现「你好」单独能中 10 次、「兮兮」单独 0 次，是全词解码路径走不通。
换 SenseVoice-small（int8 约 230MB）后同一段录音 12 句里 8 句识别出「你好，西西/嘻嘻」。

匹配策略：识别文本里出现「你好」后紧跟的 1~2 个字里，只要有 x/s 声母的近音字
（兮西嘻熙希溪喜息细析心欣新）就算唤醒。这样宽容度足够，又不会误触发
（必须真的先说「你好」）。

用法（工作目录 lvgl_ws/lv_port_pc_vscode）：
  bash scripts/wake.sh --wav ../../.tools/kws/mic_xixi.wav     # 离线跑一段录音
  bash scripts/wake.sh --mic                                   # 实时（对着麦喊）
  bash scripts/wake.sh --mic --save-wav x.wav                  # 实时并录音，便于复盘
"""

import argparse
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kws_test as K            # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
MD = os.path.join(ROOT, ".tools", "asr_sv")
MODEL = os.path.join(MD, "model.int8.onnx")
TOKENS = os.path.join(MD, "tokens.txt")

WAKE_HEAD = "你好"
# 「你好」之后允许出现的近音字（兮兮/西西/嘻嘻/熙熙/欣欣…都被 ASR 听过）
WAKE_TAIL = set("兮西嘻熙希溪喜息细析心欣新鑫")
# 整词直接命中（换成「你好小智」这类词时走这条，不依赖近音表）
WAKE_WORDS = ["小智小智", "你好小智", "你好兮兮"]

PUNCT = "，。！？、,.!? \u3000:：;；\"'“”‘’"


def clean(text):
    return "".join(c for c in text if c not in PUNCT)


def match_wake(text, words=None):
    """返回命中的唤醒词片段，否则 None。"""
    t = clean(text)
    if not t:
        return None
    for w in (words or WAKE_WORDS):
        if w and w in t:
            return w
    start = 0
    while True:
        i = t.find(WAKE_HEAD, start)
        if i < 0:
            return None
        tail = t[i + len(WAKE_HEAD): i + len(WAKE_HEAD) + 2]
        if any(c in WAKE_TAIL for c in tail):
            return (WAKE_HEAD + tail).strip()
        start = i + 1


class Recognizer:
    def __init__(self):
        import sherpa_onnx
        for f in (MODEL, TOKENS):
            if not os.path.isfile(f):
                K.die("模型文件缺失: %s\n下载见 docs 或 .tools/asr_sv 说明" % f)
        self.rec = sherpa_onnx.OfflineRecognizer.from_sense_voice(
            model=MODEL, tokens=TOKENS, num_threads=4,
            use_itn=True, language="zh", provider="cpu")
        self.cost = 0.0

    def __call__(self, pcm, rate=16000):
        t0 = time.time()
        s = self.rec.create_stream()
        s.accept_waveform(rate, np.asarray(pcm, dtype=np.float32) / 32768.0)
        self.rec.decode_stream(s)
        self.cost += time.time() - t0
        return clean(s.result.text)


def run_wav(path, rc):
    pcm, rate = K.read_wav_mono16k(path)
    segs = K.split_speech(pcm, rate)
    print("音频 %s  %.2fs，切出 %d 句\n" % (os.path.basename(path), len(pcm) / float(rate), len(segs)))
    hits = 0
    t0 = time.time()
    for i, (s, e) in enumerate(segs, 1):
        part = np.asarray(pcm[s:e], dtype=np.int16)
        text = rc(part, rate)
        w = match_wake(text)
        if w:
            hits += 1
        print("  %2d  %5.2fs-%5.2fs  %-14s %s"
              % (i, s / float(rate), e / float(rate), text, ("★ 唤醒 " + w) if w else ""))
    dur = len(pcm) / float(rate)
    print("\n唤醒 %d / %d 句 (%.0f%%)，识别耗时 %.1fs，RTF %.3f"
          % (hits, len(segs), 100.0 * hits / max(1, len(segs)), rc.cost, rc.cost / max(dur, 1e-6)))
    return 0 if hits else 2


def run_mic(rc, device, save_wav="", quiet_ms=600, min_ms=400, meter=True):
    ffmpeg, src = K.find_ffmpeg()
    if not ffmpeg:
        K.die("PATH 与常见目录里都找不到 ffmpeg（--mic 需要它抓麦克风）")
    print("[ffmpeg] %s（来源: %s）" % (ffmpeg, src))
    if not device:
        devs = K.list_devices(ffmpeg)
        if not devs:
            K.die("找不到音频采集设备")
        device = devs[0]
    import subprocess
    cmd = [ffmpeg, "-hide_banner", "-nostdin", "-loglevel", "quiet",
           "-f", "dshow", "-i", "audio=" + device,
           "-ar", "16000", "-ac", "1", "-f", "s16le", "-"]
    print("麦克风: %s" % device)
    print("对着麦克风说「你好兮兮」，说完停顿一下（Ctrl+C 退出）\n")
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, bufsize=0, stderr=subprocess.DEVNULL)
    fd = p.stdout.fileno()
    buf = bytearray()
    silence = 0.0
    allbytes = bytearray()
    nblk = 0
    hits = 0
    t_start = time.time()
    CH = K.CHUNK
    try:
        while True:
            raw = b""
            while len(raw) < CH * 2:
                piece = os.read(fd, CH * 2 - len(raw))
                if not piece:
                    print("采集流结束（ffmpeg 退出，rc=%s）" % p.poll())
                    return 1 if not hits else 0
                raw += piece
            allbytes += raw
            pcm = np.frombuffer(raw, dtype=np.int16)
            rms = float(np.sqrt((pcm.astype(np.float64) ** 2).mean()))
            buf += raw
            if rms < 150:
                silence += 0.1
            else:
                silence = 0.0
            if meter:
                bars = int(min(20, rms / 250))
                sys.stdout.write("\r[%6.1fs] %s rms=%4.0f  "
                                 % (time.time() - t_start, "#" * bars + "." * (20 - bars), rms))
                sys.stdout.flush()
            dur_ms = len(buf) / 2.0 / 16.0
            if silence * 1000 >= quiet_ms and dur_ms >= min_ms:
                part = np.frombuffer(bytes(buf), dtype=np.int16)
                text = rc(part)
                w = match_wake(text)
                nblk += 1
                if text:
                    print("\n  #%d  %s  %s"
                          % (nblk, text, ("★ 唤醒 " + w) if w else ""))
                    if w:
                        hits += 1
                        print("  ✅ 第 %d 次唤醒（%.1fs）" % (hits, time.time() - t_start))
                buf = bytearray()
                silence = 0.0
    except KeyboardInterrupt:
        print("\n\n开始监听 %.1fs，共 %d 段，唤醒 %d 次，识别耗时 %.1fs"
              % (time.time() - t_start, nblk, hits, rc.cost))
    finally:
        p.kill()
        if save_wav and allbytes:
            import wave
            with wave.open(save_wav, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(16000)
                w.writeframes(bytes(allbytes))
            print("已保存采集音频: %s（%.1fs）"
                  % (save_wav, len(allbytes) / 2.0 / 16000))
    return 0 if hits else 2


def main():
    ap = argparse.ArgumentParser(description="SenseVoice 离线唤醒")
    ap.add_argument("--wav", nargs="*", default=[], help="离线 wav（16k 单声道）")
    ap.add_argument("--mic", action="store_true", help="实时麦克风")
    ap.add_argument("--device", default="", help="DirectShow 设备名")
    ap.add_argument("--save-wav", default="", help="--mic 时把采集音频落盘")
    ap.add_argument("--no-meter", action="store_true", help="关闭电平条")
    ap.add_argument("--quiet-ms", type=int, default=600, help="判定一句话结束的静音时长")
    ap.add_argument("--words", default="", help="逗号分隔的唤醒词（默认内置一组）")
    a = ap.parse_args()
    if a.words:
        global WAKE_WORDS
        WAKE_WORDS = [w.strip() for w in a.words.split(",")]
        print("[唤醒词] %s" % " / ".join(WAKE_WORDS))
    rc = Recognizer()
    if a.mic:
        return run_mic(rc, a.device, a.save_wav, a.quiet_ms, meter=not a.no_meter)
    if not a.wav:
        K.die("要么给 wav，要么用 --mic")
    rc_ok = 0
    for p in a.wav:
        rc_ok = run_wav(p, rc)
    return rc_ok


if __name__ == "__main__":
    sys.exit(main())
