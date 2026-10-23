#!/usr/bin/env bash
# =============================================================================
# 丢包恢复验收 —— 故障注入 + 逐帧 CRC 核对 + 一次对照实验
#
# 验的不是"能不能连上"，而是"H.264 参考链断了之后，室内机会不会把垃圾画出来"。
#
# 做法：
#   ffmpeg 推流 → udp_loss.py 代理（按包序号故意丢几段）→ 室内机
#   室内机把每一帧的 CRC 写进 sidecar 日志，事后和参考码流的指纹表比对。
#
# 为什么要跑两遍（这一正一反才是完整证明）：
#   - 开恢复：脏帧必须 = 0（一帧垃圾都没被写出去），且有跳帧（真的停手等 IDR 了）
#   - 关恢复：脏帧必须 > 0（不恢复就一定会坏）
#   只跑第一遍的话，万一注入器没生效、包一个没丢，结果也是"全对"——
#   那种全绿是假的，什么也证明不了。第二遍是给第一遍的结论上保险。
#
# 用法：
#   bash scripts/loss_test.sh              # 默认推 8 秒，丢 2 段
#   bash scripts/loss_test.sh 10           # 推 10 秒
#   bash scripts/loss_test.sh 8 "60:6 690:8"   # 自定义丢包位置（单位是包序号）
#
# ⚠ 丢包位置怎么选（踩过）：
#   要丢在 **GOP 中段**，离前后两个 IDR 都远。曾经把第二段写成 300:8，
#   结果 300 号包恰好落在 frame 60 那个 IDR 上 —— 唯一的恢复希望被自己打掉了，
#   下一个 IDR 要等 3.5 秒，而放弃超时是 3 秒，于是"超时后喂脏帧"，
#   看起来像恢复策略失效，其实是测试把关键帧打死了。
#   本码流 GOP=60 帧（IDR 在 0/60/120/180/240），约 4.9 包/帧：
#     frame 13 → 包 60 左右；frame 140 → 包 690 左右。
# =============================================================================
set -u

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$SELF_DIR/bin"
FF="/c/msys64/mingw64/bin/ffmpeg.exe"
DUR="${1:-8}"
DROP_SPEC="${2:-60:6 690:8}"
SIZE="640x480"
SRC="$BIN/test_640x480.h264"
REF="$BIN/ref_640x480.yuv"

B_LOCAL=5062
A_LOCAL=7062
PROXY_PORT=16064

cd "$BIN" || { echo "没有 bin/，先 make build"; exit 1; }
if [ -d /c/msys64/mingw64/bin ]; then
    export PATH="/c/msys64/mingw64/bin:$PATH"
fi
[ -f main.exe ] && EXE=main.exe || EXE=main
[ -x "./$EXE" ] || { echo "找不到 ./$EXE"; exit 1; }

PY=python3
command -v python3 >/dev/null 2>&1 || PY=python

# ---------- 素材 ----------
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

DROP_ARGS=""
for r in $DROP_SPEC; do DROP_ARGS="$DROP_ARGS --drop $r"; done
NDROP=0
for r in $DROP_SPEC; do NDROP=$((NDROP + 1)); done

# =============================================================================
# 跑一遍。$1 = on / off（是否启用丢包恢复）
# =============================================================================
run_case() {
    local mode=$1
    local tag="loss_$mode"
    local LOG="$tag.log"
    local YUV="$tag.yuv"
    local CRCLOG="$tag.crc"
    local PROXYLOG="$tag.proxy.log"

    rm -f "$LOG" "$YUV" "$CRCLOG" "$PROXYLOG"

    # ---------- 室内机（被测） ----------
    # 关掉恢复的对照组：SIP_VIDEO_RESYNC=0 → gate 永远放行，脏帧会被写出去。
    local RESYNC_ENV=""
    [ "$mode" = "off" ] && RESYNC_ENV="SIP_VIDEO_RESYNC=0"

    env LVGL_MOCK=0 SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$B_LOCAL SIP_USER=1001 \
        SIP_AUTOANSWER=1 SIP_AUDIO_SRC=none SIP_AUDIO_SINK=drop \
        SIP_VIDEO_SINK=file SIP_VIDEO_SINK_FILE="$YUV" \
        SIP_VIDEO_FRAME_LOG="$CRCLOG" $RESYNC_ENV ${DBG:+SIP_VIDEO_DEBUG=1} \
        "./$EXE" > "$LOG" 2>&1 &
    PIDS="$PIDS $!"
    wait_log "$LOG" "listening on udp" 15 || { echo "室内机起不来"; cat "$LOG"; return 1; }

    # ---------- 客户端：只负责建通话 ----------
    # 关键：SIP_VIDEO_SINK 不能用 drop。offer 里的 m=video 行是"按接收能力"生成的，
    # sink=drop 等于"我不收视频"，于是 offer 里没有视频行，室内机那边 remote_v=0，
    # 视频端口直接不建立 —— 表现是"代理 ready、ffmpeg 正常推、室内机一个包没收到"。
    # 第一版就踩了这个（原因在一个和丢包毫无关系的地方）。
    env LVGL_MOCK=0 SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$A_LOCAL SIP_USER=2002 \
        SIP_AUTOCALL="sip:1001@127.0.0.1:$B_LOCAL" SIP_AUTOHANGUP=$((DUR + 4)) \
        SIP_AUDIO_SRC=none SIP_AUDIO_SINK=drop \
        SIP_VIDEO_SINK=file SIP_VIDEO_SINK_FILE="${tag}_a_video.yuv" \
        "./$EXE" > "${tag}_a.log" 2>&1 &
    PIDS="$PIDS $!"
    wait_log "${tag}_a.log" "media start" 20 || {
        echo "呼叫没建立"; tail -20 "${tag}_a.log"; return 1; }

    local VPORT
    VPORT=$(grab_port "$LOG" "video")
    [ -z "$VPORT" ] && { echo "室内机没开视频端口"; grep -E "video" "$LOG" | head; return 1; }

    # ---------- 丢包注入代理 ----------
    $PY "$SELF_DIR/scripts/udp_loss.py" \
        --listen "127.0.0.1:$PROXY_PORT" --to "127.0.0.1:$VPORT" \
        $DROP_ARGS --timeout 3 > "$PROXYLOG" 2>&1 &
    PIDS="$PIDS $!"
    wait_log "$PROXYLOG" "ready" 10 || { echo "注入器起不来"; cat "$PROXYLOG"; return 1; }

    sleep 1
    echo "  室内机视频口 udp/$VPORT ← 代理 udp/$PROXY_PORT（丢包规则: $DROP_SPEC）"
    "$FF" -hide_banner -loglevel error -re -i "$SRC" -t "$DUR" \
        -c copy -f rtp "rtp://127.0.0.1:$PROXY_PORT?pkt_size=1400" \
        > "${tag}_ffmpeg.log" 2>&1

    # 等挂断统计（ffmpeg 停止后还要等 SIP 那边 AUTOHANGUP 到点）
    wait_log "$LOG" "stats(" $((DUR + 14)) || echo "  （等不到挂断统计）"
    sleep 1
    cleanup; PIDS=""; sleep 2
    echo "  注入器: $(grep -E 'received=' "$PROXYLOG" | tail -1)"
}

echo "═══ 丢包恢复验收（推流 ${DUR}s，规则 ${DROP_SPEC}）═══"
echo ""
echo "──── 第一遍：关闭恢复（对照组，期望出现脏帧）────"
run_case off || exit 1
echo ""
"$PY" "$SELF_DIR/scripts/loss_analyze.py" --mode off \
    --log loss_off.log --framelog loss_off.crc --ref "$REF" --size "$SIZE" \
    --min-events $NDROP > loss_off.report 2>&1
RC_OFF=$?
cat loss_off.report

echo ""
echo "──── 第二遍：开启恢复（期望 0 脏帧 + 有跳帧 + 等到 IDR）────"
run_case on || exit 1
echo ""
"$PY" "$SELF_DIR/scripts/loss_analyze.py" --mode on \
    --log loss_on.log --framelog loss_on.crc --ref "$REF" --size "$SIZE" \
    --min-events $NDROP > loss_on.report 2>&1
RC_ON=$?
cat loss_on.report

echo ""
echo "════ 总判定 ════"
echo "  关恢复: $([ $RC_OFF -eq 0 ] && echo '✅ 符合预期（出现脏帧，说明注入有效）' || echo '❌ 不符合预期')"
echo "  开恢复: $([ $RC_ON  -eq 0 ] && echo '✅ 符合预期（0 脏帧，恢复及时）' || echo '❌ 不符合预期')"
echo ""
echo "详细报告：$BIN/loss_off.report、$BIN/loss_on.report"

[ $RC_OFF -eq 0 ] && [ $RC_ON -eq 0 ] && exit 0 || exit 1
