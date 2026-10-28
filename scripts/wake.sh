#!/usr/bin/env bash
# SenseVoice 唤醒启动器：复用 kws.sh 的解释器探测，不需要 conda activate
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KWS_SCRIPT="$HERE/wake_sv.py" exec bash "$HERE/kws.sh" "$@"
