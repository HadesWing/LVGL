#!/usr/bin/env bash
# 一条长录音 → 逐词 KWS 检出率对比。用法见 scripts/wake_batch.py 头部注释。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export KWS_SCRIPT="$HERE/wake_batch.py"
exec bash "$HERE/kws.sh" "$@"
