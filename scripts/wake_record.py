#!/usr/bin/env python3
"""唤醒词候选录音采集：按提示逐遍录音，一个词一个目录。

为什么需要：TTS 合成音无法判断"真人说这个词模型吃不吃"——实测 TTS 的
「你好兮兮」4/4 全中、外加变调/加噪压力测试后与「你好小智」表现完全一致
（都是 3/3 和 1/3），合成音在这个问题上没有区分度。只有真人录音才作数。

用法（工作目录 lvgl_ws/lv_port_pc_vscode）：
  bash scripts/rec.sh                                    # 默认 4 个候选词各 5 遍
  bash scripts/rec.sh --words 你好小智,小智小智 --repeat 8
  bash scripts/rec.sh --seconds 3 --gap 1.2 --out ../../.tools/rec

录完后跑对比：
  bash scripts/kws.sh ../../.tools/rec/你好小智/*.wav --keywords ../../.tools/kws/keywords_xiaozhi.txt
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kws_test as K            # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
DEFAULT_OUT = os.path.join(ROOT, ".tools", "rec")
DEFAULT_WORDS = "你好小智,小智小智,小智,你好兮兮"


def safe_name(w):
    return re.sub(r"[^\w\u4e00-\u9fa5]", "", w) or "word"


def record(ffmpeg, dev, out, seconds):
    cmd = [ffmpeg, "-nostdin", "-y", "-f", "dshow", "-i", "audio=%s" % dev,
           "-t", "%.1f" % seconds, "-ac", "1", "-ar", "16000",
           "-sample_fmt", "s16", out]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    return r.returncode == 0 and os.path.isfile(out) and os.path.getsize(out) > 3200


def countdown(sec, label):
    for i in range(sec, 0, -1):
        sys.stdout.write("\r  %s  %d " % (label, i))
        sys.stdout.flush()
        time.sleep(1.0)
    sys.stdout.write("\r  %s  ● 说！        \n" % label)
    sys.stdout.flush()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--words", default=DEFAULT_WORDS, help="逗号分隔的候选唤醒词")
    ap.add_argument("--repeat", type=int, default=5, help="每个词录几遍")
    ap.add_argument("--seconds", type=float, default=3.0, help="每遍录多久")
    ap.add_argument("--gap", type=float, default=1.2, help="遍与遍之间的间隔")
    ap.add_argument("--countdown", type=int, default=3, help="每次开始前倒计时秒数")
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--device", default="", help="dshow 设备名，留空取第一个")
    a = ap.parse_args()

    ffmpeg, src = K.find_ffmpeg()
    if not ffmpeg:
        K.die("需要 ffmpeg 抓麦克风，但没找到")
    dev = a.device
    if not dev:
        devs = K.list_devices(ffmpeg)
        if not devs:
            K.die("找不到音频采集设备")
        dev = devs[0][0] if isinstance(devs[0], tuple) else devs[0]
    print("[ffmpeg] %s（来源: %s）" % (ffmpeg, src))
    print("[麦克风] %s\n" % dev)

    words = [w.strip() for w in a.words.split(",") if w.strip()]
    os.makedirs(a.out, exist_ok=True)
    total = len(words) * a.repeat
    print("共 %d 个词 × %d 遍 = %d 条，每遍 %.1fs。%s"
          % (len(words), a.repeat, total, a.seconds,
             "中途可 Ctrl+C 停止，已录的会保留。" if False else "中途可 Ctrl+C 停止。"))
    print("要求：自然语速、连贯说完不要中间停顿、离麦 20-30cm、音量正常。\n")

    idx = 0
    ok = 0
    for wi, w in enumerate(words, 1):
        d = os.path.join(a.out, safe_name(w))
        os.makedirs(d, exist_ok=True)
        print("=" * 60)
        print("【%d/%d】%s" % (wi, len(words), w))
        print("=" * 60)
        for r in range(1, a.repeat + 1):
            idx += 1
            label = "%s 第 %d/%d 遍" % (w, r, a.repeat)
            countdown(a.countdown, label)
            out = os.path.join(d, "%02d.wav" % r)
            if record(ffmpeg, dev, out, a.seconds):
                ok += 1
                print("    ✔ 已存 %s" % out)
            else:
                print("    ✘ 录失败（设备被占用？）%s" % out)
            time.sleep(a.gap)

    print("\n完成：成功 %d / %d 条，目录 %s" % (ok, idx, a.out))
    print("\n下一步（逐词对比检出率）：")
    for w in words:
        d = os.path.join(a.out, safe_name(w))
        n = len(glob.glob(os.path.join(d, "*.wav")))
        if n:
            print("  bash scripts/kws.sh %s%s*.wav --keywords <对应关键词文件>"
                  % (os.path.relpath(a.out), "/" + safe_name(w) + "/"))
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
