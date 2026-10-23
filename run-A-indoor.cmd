@echo off
rem ============================================================
rem 实例 A：室内机（接听端）
rem 双击运行；响铃后用屏幕上的"接听"按钮手动接。
rem 环境变量只在本窗口生效，不会污染别的终端。
rem 端口：SIP 5062 / RTP 6062（+2 为视频口）
rem ============================================================
setlocal
cd /d "%~dp0"

set SIP_LOCAL_PORT=5062
set SIP_USER=1001
set SIP_NO_REGISTER=1

echo [实例A 室内机] SIP 5062，等对方呼叫，响铃请按"接听"...
bin\main.exe
endlocal
