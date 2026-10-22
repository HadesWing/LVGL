#!/usr/bin/env bash
# =============================================================================
# 视频接收链路验证 —— 室内机收 RTP 视频 → 重组 → 解码 → 落盘 → 与参考 YUV 比对
#
# 和音频 duo_test.sh 的思路一致，但**推流方换成 ffmpeg**。这是刻意的：
#   · 发送端用第三方实现，就不是"自己发自己收"的自证；
#   · 想验的是室内机的**接收+解码**这一段，编码/打包由 ffmpeg 保证正确，
#     出问题就一定是我们的解包/解码/落盘有问题 —— 责任边界清晰。
#
# 前置（第一次跑需要生成素材，脚本会自动做）：
#   bin/test_640x480.h264   640x480/30fps/10s，每帧叠了帧号（丢帧一眼看出）
#   bin/ref_640x480.yuv     同一段码流的参考解码结果（ffmpeg 解，逐帧基准）
#
# 用法：
#   bash scripts/duo_video_test.sh              # 默认推 8 秒
#   bash scripts/duo_video_test.sh 10           # 推 10 秒（素材全长）
# =============================================================================
set -u

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$SELF_DIR/bin"
FF="/c/msys64/mingw64/bin/ffmpeg.exe"
DUR="${1:-8}"
SIZE="640x480"
SRC="$BIN/test_640x480.h264"
REF="$BIN/ref_640x480.yuv"

cd "$BIN" || { echo "没有 bin/，先 make build"; exit 1; }
# 链接了 libavcodec 之后，运行时要能找到 avcodec-63.dll / avutil-61.dll。
# dev.sh 里也是这么做的（make run 因此不受影响），这里单独跑脚本时同样需要。
if [ -d /c/msys64/mingw64/bin ]; then
    export PATH="/c/msys64/mingw64/bin:$PATH"
fi
[ -f main.exe ] && EXE=main.exe || EXE=main
[ -x "./$EXE" ] || { echo "找不到 ./$EXE"; exit 1; }

# ---------- 素材准备 ----------
if [ ! -f "$SRC" ]; then
    echo "生成测试码流（带帧号，便于人工核对丢帧）…"
    "$FF" -y -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x480:rate=30:duration=10" \
        -vf "drawtext=fontfile='C\:/Windows/Fonts/arial.ttf':text='%{n}':fontsize=64:fontcolor=white:box=1:boxcolor=black@0.6:x=16:y=16" \
        -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
        -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 -crf 20 \
        -f h264 "$SRC" || { echo "生成码流失败（ffmpeg 路径？）"; exit 1; }
fi
if [ ! -f "$REF" ]; then
    echo "生成参考 YUV…"
    "$FF" -y -hide_banner -loglevel error -i "$SRC" \
        -pix_fmt yuv420p -f rawvideo "$REF" || exit 1
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

grab_port() {   # grab_port <log> <关键字>  → 从 "[sip] video RTP listening on udp/6064" 里取端口
    grep -o "$2.*listening on udp/[0-9]*" "$1" 2>/dev/null | head -1 | grep -o '[0-9]*$'
}

B_LOCAL=5062
A_LOCAL=7062

echo "═══ 视频接收链路测试（推流 ${DUR}s，${SIZE}）═══"
rm -f duo_v_b.log duo_v_a.log duo_b_video.yuv duo_a_video.yuv

# ---------- 室内机（被测）：收视频落盘，音频不碰设备 ----------
# LVGL_MOCK=0 关掉设备事件模拟器：它会周期性地伪造"门口机来电"，
# 在自测里会打断正在进行的通话，日志也全是噪音。
env LVGL_MOCK=0 SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$B_LOCAL SIP_USER=1001 SIP_AUTOANSWER=1 \
    SIP_AUDIO_SRC=none SIP_AUDIO_SINK=drop \
    SIP_VIDEO_SINK=file SIP_VIDEO_SINK_FILE=duo_b_video.yuv \
    "./$EXE" > duo_v_b.log 2>&1 &
PIDS="$PIDS $!"
wait_log duo_v_b.log "listening on udp" 15 || { echo "室内机起不来"; cat duo_v_b.log; exit 1; }

# ---------- 客户端：只负责呼叫，视频 sink 也开着（这样 SDP 里才 offer 视频） ----------
env LVGL_MOCK=0 SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$A_LOCAL SIP_USER=2002 \
    SIP_AUTOCALL="sip:1001@127.0.0.1:$B_LOCAL" SIP_AUTOHANGUP=$((DUR + 3)) \
    SIP_AUDIO_SRC=none SIP_AUDIO_SINK=drop \
    SIP_VIDEO_SINK=file SIP_VIDEO_SINK_FILE=duo_a_video.yuv \
    "./$EXE" > duo_v_a.log 2>&1 &
PIDS="$PIDS $!"

wait_log duo_v_a.log "media start" 20 || { echo "呼叫没建立"; tail -20 duo_v_a.log; exit 1; }

VPORT=$(grab_port duo_v_b.log "video")
if [ -z "$VPORT" ]; then
    echo "❌ 室内机没打开视频端口 —— 看 duo_v_b.log 里的 [video]/[sip] 行"
    grep -E "video|sip" duo_v_b.log | head -20
    exit 1
fi
echo "室内机视频端口 udp/$VPORT"

# 等 1 秒让通话稳定（避免 ffmpeg 的包撞在 ACK 之前）
sleep 1

echo "ffmpeg 推流 → rtp://127.0.0.1:$VPORT （pt=96, MTU 分片 1400）"
"$FF" -hide_banner -loglevel error -re -i "$SRC" -t "$DUR" \
    -c copy -f rtp "rtp://127.0.0.1:$VPORT?pkt_size=1400" > duo_v_ffmpeg.log 2>&1
FFRC=$?
[ $FFRC -ne 0 ] && { echo "⚠ ffmpeg 退出码 $FFRC"; tail -10 duo_v_ffmpeg.log; }

sleep 2

# 等室内机挂断并打印收尾统计再 kill —— 直接 kill 会丢掉 [video] stats 那一行，
# 而"没丢片、没错序"的证据恰恰在那里（只看 bit-exact 看不出有没有重传补回来）。
wait_log duo_v_b.log "stats(" $((DUR + 12)) || echo "（等不到挂断统计，可能是被强制结束）"
sleep 1
cleanup; PIDS=""; sleep 1
trap - EXIT INT TERM

echo ""
echo "──── 室内机（被测）────"
grep -E "^\[video\]|^\[rtp\] (video )?stats" duo_v_b.log | tail -12
echo "──── 客户端（呼叫方）────"
grep -E "^\[video\]|^\[rtp\] (video )?stats" duo_v_a.log | tail -6

echo ""
echo "──── 落盘 ────"
ls -l duo_b_video.yuv 2>/dev/null || echo "（室内机没有落盘文件）"
echo ""

# ---------- 比对 ----------
if [ ! -f duo_b_video.yuv ]; then
    echo "❌ 没有落盘，解码链路没跑起来"
    exit 1
fi

PY=python3
command -v python3 >/dev/null 2>&1 || PY=python
$PY "$SELF_DIR/scripts/video_compare.py" "$REF" duo_b_video.yuv \
    --size "$SIZE" --align
RC=$?

echo ""
echo "想肉眼看画面的话："
echo "  /c/msys64/mingw64/bin/ffplay -f rawvideo -pixel_format yuv420p -video_size $SIZE duo_b_video.yuv"
exit $RC
