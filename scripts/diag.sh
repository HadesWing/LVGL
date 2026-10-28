#!/usr/bin/env bash
# 录音质量体检。解释器复用 kws.sh（会自动挑装了 numpy/sherpa 的那个），不需要 conda activate。
#
#   bash scripts/diag.sh ../../.tools/kws/mic_xixi.wav
#   bash scripts/diag.sh a.wav b.wav
set -u
D="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export KWS_SCRIPT="$D/wav_diag.py"
exec bash "$D/kws.sh" "$@"
