#!/usr/bin/env bash
# =============================================================================
# M5 长稳压测 —— N 轮"呼入 → 接通 → 音视频推流 → 挂断"循环
#
# 【为什么 A 常驻、B 每轮重启】
#   泄漏要盯"不重启的那个进程"。A（被测室内机）从第 0 秒活到最后一秒，
#   它的内存曲线才有判泄漏的意义；B（呼叫端）只是流量发生器，每轮重开。
#
# 【每轮覆盖的路径】
#   INVITE/180/200/ACK/BYE 全套信令、SDP 音视频协商、G.711 双向（B 推 wav，
#   A 落盘）、H.264 推流上屏（ffmpeg → 解码 → 上屏）、挂断忙音、
#   落盘文件每轮重开。轮间 A 回到 online，状态机完整走 N 个来回。
#
# 【判据】见 stress_analyze.py：轮数齐全 / 0 崩溃 / 内存斜率 / 碎片率 /
#   每轮音频峰值非零 / 视频零脏帧零错误。
#
# 用法：
#   bash scripts/stress_test.sh              # 默认 20 轮 × 通话 5s ≈ 4 分钟
#   bash scripts/stress_test.sh 50 6         # 50 轮 × 通话 6s
# =============================================================================
set -u

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$SELF_DIR/bin"
ROUNDS="${1:-20}"
HOLD="${2:-5}"
PUSH_SEC=4            # 每轮视频推流秒数

cd "$BIN" || { echo "没有 bin/，先 make build"; exit 1; }
if [ -d /c/msys64/mingw64/bin ]; then
    export PATH="/c/msys64/mingw64/bin:$PATH"
fi
[ -f main.exe ] && EXE=main.exe || EXE=main
[ -x "./$EXE" ] || { echo "找不到 ./$EXE"; exit 1; }

FF="/c/msys64/mingw64/bin/ffmpeg.exe"
SRC="$BIN/test_640x480.h264"
if [ ! -f "$SRC" ]; then
    echo "生成测试码流…"
    "$FF" -y -hide_banner -loglevel error \
        -f lavfi -i "testsrc2=size=640x480:rate=30:duration=10" \
        -vf "drawtext=fontfile='C\:/Windows/Fonts/arial.ttf':text='%{n}':fontsize=64:fontcolor=white:box=1:boxcolor=black@0.6:x=16:y=16" \
        -c:v libx264 -profile:v baseline -level 3.1 -pix_fmt yuv420p \
        -g 60 -keyint_min 60 -sc_threshold 0 -bf 0 -crf 20 \
        -f h264 "$SRC" || { echo "生成码流失败"; exit 1; }
fi
[ -f test_8k.wav ] || python "$SELF_DIR/scripts/gen_test_wav.py" test_8k.wav || true

B_LOCAL=7072
A_LOCAL=5072

wait_log() {          # wait_log <文件> <关键字> <超时秒>
    local f=$1 pat=$2 t=${3:-20} i=0
    while [ $i -lt $((t * 10)) ]; do
        grep -q "$pat" "$f" 2>/dev/null && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

# 等关键字的"出现次数"超过 before —— 全文件 grep 会匹配上一轮的旧记录，
# 把"还没发生的挂断"当成立即通过，下一轮就把 B 杀了，A 卡在 INCALL
# 后面全部 486（实测踩过）。必须按增量等。
wait_count() {        # wait_count <文件> <关键字> <至少次数> <超时秒>
    local f=$1 pat=$2 want=$3 t=${4:-20} i=0 c
    while [ $i -lt $((t * 10)) ]; do
        c=$(grep -ac "$pat" "$f" 2>/dev/null); c=${c:-0}
        [ "$c" -ge "$want" ] && return 0
        sleep 0.1; i=$((i + 1))
    done
    return 1
}

grab_port() {
    grep -a "video RTP listening" "$1" 2>/dev/null | head -1 | grep -o '[0-9]*$'
}

# ---------- 被测 A：常驻全程，内存曲线从这里出 ----------
rm -f stress_a.log stress_mem.csv stats.csv stress_rounds.txt
: > stress_rounds.txt

echo "═══ M5 压测：${ROUNDS} 轮 × 通话 ${HOLD}s ═══"
env LVGL_MOCK=0 SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$A_LOCAL SIP_USER=1001 SIP_AUTOANSWER=1 \
    SIP_AUDIO_SRC=none SIP_AUDIO_SINK=file SIP_AUDIO_SINK_FILE=stress_rx.wav \
    SIP_VIDEO_SINK=disp LVGL_STATS=1 \
    "./$EXE" > stress_a.log 2>&1 &
A_PID=$!
wait_log stress_a.log "listening on udp" 20 || { echo "❌ A 起不来"; kill -9 $A_PID 2>/dev/null; exit 1; }

FAIL=0
cleanup_a() { kill "$A_PID" 2>/dev/null; sleep 1; kill -9 "$A_PID" 2>/dev/null; }
trap cleanup_a EXIT INT TERM

for r in $(seq 1 "$ROUNDS"); do
    BEFORE_START=$(grep -ac "media start" stress_a.log 2>/dev/null); BEFORE_START=${BEFORE_START:-0}
    BEFORE_HANG=$(grep -ac "stats(hangup)" stress_a.log 2>/dev/null); BEFORE_HANG=${BEFORE_HANG:-0}

    env LVGL_MOCK=0 SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$B_LOCAL SIP_USER=2002 \
        SIP_AUTOCALL="sip:1001@127.0.0.1:$A_LOCAL" SIP_AUTOHANGUP="$HOLD" \
        SIP_AUDIO_SRC=file SIP_AUDIO_SRC_FILE=test_8k.wav SIP_AUDIO_SINK=drop \
        "./$EXE" > "stress_b_$r.log" 2>&1 &
    B_PID=$!

    if ! wait_count stress_a.log "media start" $((BEFORE_START + 1)) 20; then
        echo "❌ 第 $r 轮：呼叫没建立"
        FAIL=$((FAIL + 1)); kill -9 $B_PID 2>/dev/null; sleep 2
        continue
    fi

    # 视频推流：A 的视频端口每轮重新 bind，端口号固定
    VPORT=$(grab_port stress_a.log)
    if [ -n "$VPORT" ]; then
        "$FF" -hide_banner -loglevel error -re -i "$SRC" -t "$PUSH_SEC" \
            -c copy -f rtp "rtp://127.0.0.1:$VPORT?pkt_size=1400" > /dev/null 2>&1
    fi

    # 等本轮"新的"挂断统计（B AUTOHANGUP -> BYE -> A stats(hangup)）
    if wait_count stress_a.log "stats(hangup)" $((BEFORE_HANG + 1)) $((HOLD + 20)); then
        echo "$r" >> stress_rounds.txt
    else
        echo "❌ 第 $r 轮：等不到挂断统计"
        FAIL=$((FAIL + 1))
    fi
    kill "$B_PID" 2>/dev/null
    sleep 1
    kill -9 "$B_PID" 2>/dev/null
    sleep 1     # 轮间隙：忙音/状态回稳
done

cleanup_a
trap - EXIT INT TERM
mv -f stats.csv stress_mem.csv 2>/dev/null

echo ""
echo "完成 $ROUNDS 轮，失败 $FAIL 轮。判据分析："
python "$SELF_DIR/scripts/stress_analyze.py" \
    --log stress_a.log --csv stress_mem.csv \
    --rounds "$ROUNDS" --rounds-done stress_rounds.txt
RC=$?
exit $RC
