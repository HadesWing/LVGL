#!/usr/bin/env bash
# 多引擎 ASR 横向对比。用法见 scripts/asr_compare.py 头部注释。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export KWS_SCRIPT="$HERE/asr_compare.py"
exec bash "$HERE/kws.sh" "$@"
