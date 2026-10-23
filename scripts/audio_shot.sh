#!/usr/bin/env bash
# 抓"纯语音通话"界面截图：室内机(SIP_VIDEO_SINK=drop → SDP 视频回 0 → UI 纯语音态)
set -u
cd "C:/Users/hades/WorkBuddy/面试/lvgl_ws/lv_port_pc_vscode/bin" || exit 1
export PATH="/c/msys64/mingw64/bin:$PATH"
rm -f audio_call_screen.ppm audio_a.log audio_b.log

env SIP_NO_REGISTER=1 SIP_LOCAL_PORT=5062 SIP_USER=1001 SIP_AUTOANSWER=1 \
    SIP_AUDIO_SRC=none SIP_AUDIO_SINK=drop SIP_VIDEO_SINK=drop \
    SIP_AUTOHANGUP=14 SIP_VIDEO_DISP_SHOT=audio_call_screen.ppm SIP_VIDEO_DISP_SHOT_AT=3 \
    ./main.exe > audio_a.log 2>&1 &
APID=$!
sleep 3
env SIP_NO_REGISTER=1 SIP_LOCAL_PORT=7062 SIP_USER=2002 \
    SIP_AUTOCALL="sip:1001@127.0.0.1:5062" SIP_AUTOHANGUP=10 \
    SIP_AUDIO_SRC=file SIP_AUDIO_SRC_FILE=test_8k.wav SIP_AUDIO_SINK=drop \
    ./main.exe > audio_b.log 2>&1 &
BPID=$!

for i in $(seq 1 200); do
    [ -f audio_call_screen.ppm ] && break
    kill -0 $APID 2>/dev/null || break
    sleep 0.5
done
sleep 1
kill $APID $BPID 2>/dev/null
wait 2>/dev/null
echo "=== A 关键日志 ==="
grep -E "抓屏|INVITE|200|answer|video| ended" audio_a.log | head -10
ls -la audio_call_screen.ppm 2>/dev/null || echo "NO_PPM"
