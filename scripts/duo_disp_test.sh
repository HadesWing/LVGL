#!/usr/bin/env bash
# =============================================================================
# 视频上屏链路验证 —— RTP → 解包 → 解码 → 缩放 → RGB565 → LVGL 绘制 → 抓屏比对
#
# 与 duo_video_test.sh 的区别只有一处，但很关键：
#   duo_video_test  验到"解码器输出的 YUV 和参考帧逐字节相同"为止；
#   duo_disp_test   再往后走三步（YUV→RGB565 收缩、lv_image 绘制、屏幕合成），
#                   靠"抓屏 → 逐像素比"来验，而不是靠肉眼看。
#
# 为什么非要抓屏：上屏这一路的故障长得都像"信号不好"——
#   花屏（FU-A 丢片）、偏色（U/V 串了）、纵向翻转/斜切（linesize 用法错）、
#   只画半边（缩放算错）、发灰（有限量程当全量程）。
#   320x480 的小屏上人眼分不清这些，逐像素比能指出第几行第几列差多少。
#
# 用法：
#   bash scripts/duo_disp_test.sh          # 默认推 6 秒，第 45 帧抓屏
#   bash scripts/duo_disp_test.sh 8 60     # 推 8 秒，第 60 帧抓屏
# =============================================================================
set -u

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$SELF_DIR/bin"
FF="/c/msys64/mingw64/bin/ffmpeg.exe"
DUR="${1:-6}"
SHOT_AT="${2:-45}"
SIZE="640x480"
SRC="$BIN/test_640x480.h264"
REF="$BIN/ref_640x480.yuv"
SHOT="$BIN/duo_disp_screen.ppm"

cd "$BIN" || { echo "没有 bin/，先 make build"; exit 1; }
if [ -d /c/msys64/mingw64/bin ]; then
    export PATH="/c/msys64/mingw64/bin:$PATH"
fi
[ -f main.exe ] && EXE=main.exe || EXE=main
[ -x "./$EXE" ] || { echo "找不到 ./$EXE"; exit 1; }

# ---------- 素材：和 duo_video_test 共用，缺了就现场生成 ----------
if [ ! -f "$SRC" ]; then
    echo "生成测试码流（带帧号）…"
    "$FF" -y -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x480:rate=30:duration=10" \
        -vf "drawtext=fontfile='C\:/Windows/Fonts/arial.ttf':text='%{n}':fontsize=64:fontcolor=white:box=1:boxcolor=black@0.6:x=16:y=16" \
        -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
        -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 -crf 20 \
        -f h264 "$SRC" || { echo "生成码流失败"; exit 1; }
fi
if [ ! -f "$REF" ]; then
    echo "生成参考 YUV…"
    "$FF" -y -hide_banner -loglevel error -i "$SRC" -pix_fmt yuv420p -f rawvideo "$REF" || exit 1
fi

PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    sleep 1
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
}
trap cleanup EXIT INT TERM

wait_log() {
    local f=$1 pat=$2 t=${3:-15} i=0
    while [ $i -lt $((t * 10)) ]; do
        grep -q "$pat" "$f" 2>/dev/null && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

grab_port() {
    grep -o "$2.*listening on udp/[0-9]*" "$1" 2>/dev/null | head -1 | grep -o '[0-9]*$'
}

B_LOCAL=5072
A_LOCAL=7072

echo "═══ 视频上屏链路测试（推流 ${DUR}s，第 ${SHOT_AT} 帧抓屏）═══"
rm -f duo_d_b.log duo_d_a.log "$SHOT" duo_d_b_video.yuv

# ---------- 室内机（被测）：收视频 → 上屏 ----------
# SIP_VIDEO_DISP_SHOT 打开抓屏：收到第 SHOT_AT 帧后延迟 80ms 把整个屏幕存成 PPM。
# 那 80ms 是必须的 —— 刚 set_src 完就抓，抓到的是上一帧，比对结果会"差一帧"，
# 很容易被误判成解码延迟。
env LVGL_MOCK=0 SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$B_LOCAL SIP_USER=1001 SIP_AUTOANSWER=1 \
    SIP_AUDIO_SRC=none SIP_AUDIO_SINK=drop \
    SIP_VIDEO_SINK=disp \
    SIP_VIDEO_DISP_SHOT="duo_disp_screen.ppm" SIP_VIDEO_DISP_SHOT_AT=$SHOT_AT \
    "./$EXE" > duo_d_b.log 2>&1 &
PIDS="$PIDS $!"
wait_log duo_d_b.log "listening on udp" 15 || { echo "室内机起不来"; cat duo_d_b.log; exit 1; }

# ---------- 客户端：只负责建立通话（sink 开着，SDP 里才会 offer 视频） ----------
env LVGL_MOCK=0 SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$A_LOCAL SIP_USER=2002 \
    SIP_AUTOCALL="sip:1001@127.0.0.1:$B_LOCAL" SIP_AUTOHANGUP=$((DUR + 3)) \
    SIP_AUDIO_SRC=none SIP_AUDIO_SINK=drop \
    SIP_VIDEO_SINK=file SIP_VIDEO_SINK_FILE=duo_d_b_video.yuv \
    "./$EXE" > duo_d_a.log 2>&1 &
PIDS="$PIDS $!"

wait_log duo_d_a.log "media start" 20 || { echo "呼叫没建立"; tail -20 duo_d_a.log; exit 1; }

VPORT=$(grab_port duo_d_b.log "video")
if [ -z "$VPORT" ]; then
    echo "❌ 室内机没打开视频端口"
    grep -E "video|sip" duo_d_b.log | head -20
    exit 1
fi
echo "室内机视频端口 udp/$VPORT"

sleep 1
echo "ffmpeg 推流 → rtp://127.0.0.1:$VPORT"
"$FF" -hide_banner -loglevel error -re -i "$SRC" -t "$DUR" \
    -c copy -f rtp "rtp://127.0.0.1:$VPORT?pkt_size=1400" > duo_d_ffmpeg.log 2>&1
[ $? -ne 0 ] && { echo "⚠ ffmpeg 出错"; tail -10 duo_d_ffmpeg.log; }

# 等抓屏落盘（第 SHOT_AT 帧 ≈ SHOT_AT/30 秒处，留足余量）
sleep 2
wait_log duo_d_b.log "抓屏 →" 10 || echo "（等不到抓屏记录）"
wait_log duo_d_b.log "stats(" $((DUR + 12)) || echo "（等不到挂断统计）"
sleep 1
cleanup; PIDS=""; sleep 1
trap - EXIT INT TERM

echo ""
echo "──── 室内机（被测）────"
grep -E "^\[video\]|^\[disp\]" duo_d_b.log | tail -14
echo ""
echo "──── 客户端 ────"
grep -E "^\[rtp\] (video )?stats" duo_d_a.log | tail -4

echo ""
if [ ! -f "$SHOT" ]; then
    echo "❌ 没有抓屏文件 —— 上屏链路没跑起来"
    grep -E "\[video\]|\[disp\]" duo_d_b.log | tail -20
    exit 1
fi

PY=python3
command -v python3 >/dev/null 2>&1 || PY=python
echo "──── 逐像素比对 ────"
$PY "$SELF_DIR/scripts/disp_compare.py" --log duo_d_b.log --ref "$REF" --src "$SIZE" \
    --dump-diff duo_disp_diff.ppm
RC=$?

echo ""
echo "想肉眼看抓屏：直接打开 $SHOT（PPM，Windows 照片可能不认，用 ffmpeg 转 png）"
echo "  $FF -y -i $SHOT duo_disp_screen.png"
exit $RC
