#!/usr/bin/env bash
# 唤醒词测试启动器：自动挑一个装了 sherpa-onnx 的 Python 解释器
#
# 背景：Windows 上常同时存在多个 python（conda 环境 / 系统 Python / MSYS2 / 各种虚拟环境），
#      sherpa-onnx 只装在其中一个里。本脚本按顺序探测，用第一个 import 得通的跑。
#      探测走绝对路径，**不需要 conda activate**。
#
# 首选 conda 环境 kws（D:\Program Files\anaconda3\envs\kws）：
#   conda activate kws && python -m pip install sherpa-onnx sherpa-onnx-core
#
# 用法（参数原样透传给被测脚本）：
#   ./scripts/kws.sh --mic                       # KWS 实时
#   ./scripts/kws.sh ../../.tools/kws/test_wavs/*.wav
#   ./scripts/kws.sh --mic --save-wav x.wav --autotune
#   ./scripts/kws.sh --grid x.wav                 # 阈值×加分 网格
#   ./scripts/kws.sh --segments x.wav             # 逐句诊断
#   KWS_SCRIPT=.../asr_wake_test.py ./scripts/kws.sh --wav x.wav
#   KWS_PYTHON=/c/Python314/python.exe ./scripts/kws.sh --mic   # 强制指定解释器
#   KWS_ENV=other ./scripts/kws.sh --mic                        # 换一个 conda 环境名

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"   # 面试 工作区根
MD="$ROOT/.tools/kws"
KW="$MD/keywords_xixi.txt"
ENVNAME="${KWS_ENV:-kws}"
TARGET="${KWS_SCRIPT:-$SCRIPT_DIR/kws_test.py}"

# 只有 kws_test.py 需要模型目录和关键词文件；asr_wake_test.py 自带默认值
EXTRA=()
if [ "$(basename "$TARGET")" = "kws_test.py" ]; then
  if [ ! -d "$MD" ]; then
    echo "错误: 模型目录不存在: $MD" >&2
    exit 1
  fi
  EXTRA=(--model-dir "$MD" --keywords "$KW")
fi

CANDS=()
[ -n "${KWS_PYTHON:-}" ] && CANDS+=("$KWS_PYTHON")

# 1) 当前已激活的就是目标 conda 环境
if [ -n "${CONDA_PREFIX:-}" ] && [ "${CONDA_DEFAULT_ENV:-}" = "$ENVNAME" ]; then
  CANDS+=("$CONDA_PREFIX/python.exe")
fi

# 2) 已知 conda 根目录下的 <envs>/<ENVNAME>
for root in "/d/Program Files/anaconda3" "/c/ProgramData/anaconda3" \
            "/c/ProgramData/miniconda3" "$HOME/anaconda3" "$HOME/miniconda3"; do
  CANDS+=("$root/envs/$ENVNAME/python.exe")
done

# 3) 兜底：系统 Python / WorkBuddy 内置 / MSYS2
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
  echo "修复（推荐装进 conda 环境 $ENVNAME）：" >&2
  echo "  conda activate $ENVNAME && python -m pip install sherpa-onnx sherpa-onnx-core" >&2
  exit 1
fi

echo "[kws] 解释器: $PY"
echo "[kws] 被测脚本: $(basename "$TARGET")"
if [ "${#EXTRA[@]}" -gt 0 ]; then echo "[kws] 模型目录: $MD"; fi

# --mic 模式要靠 ffmpeg 抓 DirectShow 设备。
# 优先用"解释器同环境"的 ffmpeg（conda 环境装在 <env>/Library/bin），这样不必 activate；
# 没有才退到 MSYS2 安装。真正的选择由被测脚本的 find_ffmpeg() 完成，这里只做 PATH 兜底。
PYENV_DIR="$(dirname "$PY")"
HAVE_ENV_FFMPEG=0
for d in "$PYENV_DIR/Library/bin" "$PYENV_DIR/bin" "$PYENV_DIR"; do
  if [ -x "$d/ffmpeg.exe" ]; then HAVE_ENV_FFMPEG=1; break; fi
done
if [ "$HAVE_ENV_FFMPEG" = 0 ] && ! command -v ffmpeg >/dev/null 2>&1; then
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

exec "$PY" -u "$TARGET" "${EXTRA[@]}" "$@"
