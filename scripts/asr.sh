#!/usr/bin/env bash
# 流式 ASR 唤醒词检测（替代 KWS）。解释器探测复用 kws.sh，不需要 conda activate。
#
# 用法（工作目录 lvgl_ws/lv_port_pc_vscode）：
#   bash scripts/asr.sh --wav ../../.tools/kws/mic_xixi.wav
#   bash scripts/asr.sh --wav ../../.tools/kws/mic_xixi.wav --segments
#   bash scripts/asr.sh --mic
#   bash scripts/asr.sh --wav x.wav --wake "你好兮兮,小爱同学"
set -u
D="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KWS_SCRIPT="$D/asr_wake_test.py"
exec bash "$D/kws.sh" "$@"
