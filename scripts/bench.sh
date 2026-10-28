#!/usr/bin/env bash
# 音频加工台启动器：复用 kws.sh 的解释器探测，不需要 conda activate
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KWS_SCRIPT="$HERE/audio_bench.py" exec bash "$HERE/kws.sh" "$@"
