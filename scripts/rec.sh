#!/usr/bin/env bash
# 唤醒词候选录音采集启动器。用法见 scripts/wake_record.py 头部注释。
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
export KWS_SCRIPT="$HERE/wake_record.py"
exec bash "$HERE/kws.sh" "$@"
