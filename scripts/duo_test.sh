#!/usr/bin/env bash
# =============================================================================
# 双端互通测试（duo test）—— 一台机器上跑两个实例，等价于"室内机 + PC 客户端"
#
# 为什么需要它：
#   单实例自环（SIP_RTP_ECHO 或 src=file→sink=file）只能证明"编码→RTP→解码"
#   这条链自己跟自己自洽。它证明不了：INVITE/SDP 协商、对称 RTP 回填、
#   两端各自的时间戳/序号基准能不能对上 —— 那些必须有两个独立进程才暴露。
#
# 两个方向各跑一次，全程不需要人、不需要声卡、不需要手机：
#   方向 1  室内机发 → 客户端收   （室内机关扬声器：sink=drop）
#   方向 2  客户端发 → 室内机收   （室内机关 mic  ：src=none）
#
# 用法：
#   bash scripts/duo_test.sh            # 每个方向 12 秒（默认）
#   bash scripts/duo_test.sh 20         # 每个方向 20 秒
#   bash scripts/duo_test.sh 12 keep    # 保留 bin/duo_*.wav 供试听
#
# 产物（都在 bin/ 下）：
#   duo_a_rx.wav  客户端收到的（验方向 1）
#   duo_b_rx.wav  室内机收到的（验方向 2）
#   duo_a1.log duo_b1.log duo_a2.log duo_b2.log
# =============================================================================
set -u

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$SELF_DIR/bin"
DUR="${1:-12}"
KEEP="${2:-}"

cd "$BIN" || { echo "没有 bin/，先编译：make build"; exit 1; }
[ -f main.exe ] && EXE=main.exe || EXE=main
[ -x "./$EXE" ] || { echo "找不到 ./$EXE，先执行 make build"; exit 1; }

# ---------- 端口规划：必须手动错开 ----------
# 程序里 RTP 端口 = local_port + 1000（往上找偶数口）。
#   室内机 local=5062 → SIP 5062 / RTP 6062
#   客户端 local=7062 → SIP 7062 / RTP 8062
# 曾经踩过：客户端 local 用 6062，SIP 端口正好压在室内机的 RTP 段上，
# 第二个实例直接 "bind 6062 failed" 起不来。
B_LOCAL=5062
A_LOCAL=7062
B_RTP=$((B_LOCAL + 1000))

SRC_WAV=test_8k.wav
[ -f "$SRC_WAV" ] || python3 "$SELF_DIR/scripts/gen_test_wav.py" "$SRC_WAV" \
    || { echo "缺 $SRC_WAV，且生成失败"; exit 1; }

PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    sleep 1
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
}
trap cleanup EXIT INT TERM

# 轮询日志等就绪，比 sleep 猜时间可靠（SDL 初始化 1~3s 波动很大）
wait_log() {          # wait_log <文件> <关键字> <超时秒>
    local f=$1 pat=$2 t=${3:-15} i=0
    while [ $i -lt $((t * 10)) ]; do
        grep -q "$pat" "$f" 2>/dev/null && return 0
        sleep 0.1; i=$((i + 1))
    done
    echo "   ✗ 等不到 \"$pat\"（$(basename "$f")）"; return 1
}

# 起一个实例：start <日志> <env...> -- 
start() {
    local log=$1; shift
    env "$@" "./$EXE" > "$log" 2>&1 &
    PIDS="$PIDS $!"
}

stat_line() { grep -E "^\[rtp\] stats|落盘完成" "$1" | tr '\n' ' '; }

echo "═══ 双端互通测试：每方向 ${DUR}s ═══"
echo "   室内机 SIP/$B_LOCAL RTP/$B_RTP    客户端 SIP/$A_LOCAL RTP/$((A_LOCAL + 1000))"
echo ""

# =============================================================================
# 方向 1：室内机发 → 客户端收（验证"室内机上行 + 客户端下行"）
# =============================================================================
echo "── 方向 1/2：室内机 → 客户端（室内机 sink=drop 关扬声器）"
rm -f duo_a_rx.wav duo_a1.log duo_b1.log
start duo_a1.log SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$A_LOCAL SIP_USER=2002 \
          SIP_AUTOANSWER=1 SIP_AUDIO_SRC=none SIP_AUDIO_SINK=file \
          SIP_AUDIO_SINK_FILE=duo_a_rx.wav
wait_log duo_a1.log "listening on udp" 15 || exit 1

start duo_b1.log SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$B_LOCAL SIP_USER=1001 \
      SIP_AUTOCALL="sip:2002@127.0.0.1:$A_LOCAL" SIP_AUTOHANGUP=$DUR \
      SIP_AUDIO_SRC=file SIP_AUDIO_SRC_FILE=$SRC_WAV SIP_AUDIO_SINK=drop
wait_log duo_b1.log "call ended\|stats(" $((DUR + 20)) || true
cleanup; PIDS=""; trap cleanup EXIT INT TERM; sleep 1

echo "   室内机: $(stat_line duo_b1.log)"
echo "   客户端: $(stat_line duo_a1.log)"
echo ""

# =============================================================================
# 方向 2：客户端发 → 室内机收（验证"客户端上行 + 室内机下行"）
# =============================================================================
echo "── 方向 2/2：客户端 → 室内机（室内机 src=none 关 mic）"
rm -f duo_b_rx.wav duo_a2.log duo_b2.log
start duo_b2.log SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$B_LOCAL SIP_USER=1001 \
      SIP_AUTOANSWER=1 SIP_AUDIO_SRC=none SIP_AUDIO_SINK=file \
      SIP_AUDIO_SINK_FILE=duo_b_rx.wav
wait_log duo_b2.log "listening on udp" 15 || exit 1

start duo_a2.log SIP_NO_REGISTER=1 SIP_LOCAL_PORT=$A_LOCAL SIP_USER=2002 \
      SIP_AUTOCALL="sip:1001@127.0.0.1:$B_LOCAL" SIP_AUTOHANGUP=$DUR \
      SIP_AUDIO_SRC=file SIP_AUDIO_SRC_FILE=$SRC_WAV SIP_AUDIO_SINK=drop
wait_log duo_a2.log "call ended\|stats(" $((DUR + 20)) || true
cleanup; PIDS=""; sleep 1

echo "   客户端: $(stat_line duo_a2.log)"
echo "   室内机: $(stat_line duo_b2.log)"
echo ""

# =============================================================================
# 结论：数量（RTP 包数）+ 波形（SNR）
# =============================================================================
echo "═══ 结论 ═══"
FAIL=0
check() {   # check <方向名> <源 wav> <收到的 wav>
    local name=$1 src=$2 rx=$3
    if [ ! -f "$rx" ]; then
        echo "   ✗ $name：没有落盘文件 —— 收端一个包都没收到"; FAIL=1; return
    fi
    if ! command -v python3 >/dev/null 2>&1; then
        echo "   ? $name：有落盘 $(stat -c%s "$rx" 2>/dev/null || stat -f%z "$rx") 字节（无 python3，跳过 SNR）"
        return
    fi
    local out; out=$(python3 "$SELF_DIR/scripts/audio_compare.py" "$src" "$rx" 2>&1)
    local snr; snr=$(echo "$out" | grep -o "SNR: [0-9.]* dB" | head -1)
    local gain; gain=$(echo "$out" | grep -o "增益: [0-9.]*" | head -1)
    local ok; ok=$(echo "$out" | grep -c "通路正确")
    if [ "$ok" -ge 1 ]; then
        echo "   ✓ $name：$snr  $gain  → 通路正确"
    else
        echo "   ✗ $name：$snr  $gain  → 失真"; echo "$out" | sed 's/^/       /'; FAIL=1
    fi
}
check "室内机 → 客户端" "$SRC_WAV" duo_a_rx.wav
check "客户端 → 室内机" "$SRC_WAV" duo_b_rx.wav

# 交互时长核对：落盘秒数应≈通话秒数（明显偏短=大量丢包或提前挂断）
for f in duo_a_rx.wav duo_b_rx.wav; do
    [ -f "$f" ] || continue
    sz=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f")
    sec=$(( (sz - 44) / 16000 ))
    echo "   · $f ≈ ${sec}s（通话 ${DUR}s，偏少超过 2s 说明有丢包）"
done

if [ "$KEEP" != "keep" ]; then
    echo ""
    echo "（要留着试听就别加 keep 参数删掉；文件名已区分方向：duo_a_rx=客户端收, duo_b_rx=室内机收）"
fi
echo ""
[ $FAIL -eq 0 ] && echo "✅ 双向音频通路通过" || echo "❌ 有方向未通过，看 bin/duo_*.log"
exit $FAIL
