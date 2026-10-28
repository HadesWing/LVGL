#!/usr/bin/env bash
# 唤醒词测试启动器：自动挑一个装了 sherpa-onnx 的 Python 解释器
#
# 背景：Windows 上常同时存在多个 python（系统 Python / MSYS2 Python / 各种虚拟环境），
#      sherpa-onnx 只装在其中一个里。本脚本按顺序探测，用第一个 import 得通的跑。
#
# 用法（参数原样透传给 kws_test.py）：
#   ./scripts/kws.sh --mic
#   ./scripts/kws.sh ../.tools/kws/test_wavs/*.wav
#   ./scripts/kws.sh --mic --threshold 0.15
#   KWS_PYTHON=/c/Python314/python.exe ./scripts/kws.sh --mic   # 强制指定解释器

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"   # 面试 工作区根
MD="$ROOT/.tools/kws"
KW="$MD/keywords_xixi.txt"

if [ ! -d "$MD" ]; then
  echo "错误: 模型目录不存在: $MD" >&2
  exit 1
fi

# 候选解释器：显式指定 > 系统 Python > WorkBuddy 内置 Python > MSYS2
CANDS=()
[ -n "${KWS_PYTHON:-}" ] && CANDS+=("$KWS_PYTHON")
CANDS+=("/c/Python314/python.exe")
for p in "$HOME"/.workbuddy/binaries/python/versions/*/python.exe; do
  [ -x "$p" ] && CANDS+=("$p")
done
CANDS+=("/c/msys64/mingw64/bin/python.exe" "/c/msys64/usr/bin/python3.exe")

PY=""
for c in "${CANDS[@]}"; do
  [ -x "$c" ] || continue
  if "$c" -c "import sherpa_onnx" >/dev/null 2>&1; then
    PY="$c"
    break
  fi
done

if [ -z "$PY" ]; then
  echo "错误: 找不到装了 sherpa-onnx 的 Python。候选都试过了：" >&2
  for c in "${CANDS[@]}"; do echo "  $c" >&2; done
  echo "" >&2
  echo "修复：/c/Python314/python.exe -m pip install sherpa-onnx sherpa-onnx-core" >&2
  exit 1
fi

echo "[kws] 解释器: $PY"
echo "[kws] 模型目录: $MD"

# --mic 模式要靠 ffmpeg 抓 DirectShow 设备，缺了就补一条 MSYS2 路径进来
if ! command -v ffmpeg >/dev/null 2>&1; then
  for d in /c/msys64/mingw64/bin /c/msys64/usr/bin; do
    if [ -x "$d/ffmpeg.exe" ]; then
      export PATH="$PATH:$d"
      echo "[kws] ffmpeg: $d（已追加到 PATH）"
      break
    fi
  done
fi

export PYTHONUTF8=1
export PYTHONIOENCODING=utf-8

exec "$PY" -u "$SCRIPT_DIR/kws_test.py" \
  --model-dir "$MD" \
  --keywords "$KW" \
  "$@"
