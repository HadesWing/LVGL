#!/usr/bin/env bash
# 节奏/音色对齐台启动器。用法见 scripts/prosody_match.py 头部注释。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export KWS_SCRIPT="$HERE/prosody_match.py"
exec bash "$HERE/kws.sh" "$@"
