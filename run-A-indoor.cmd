@echo off
rem ============================================================
rem 实例 A：室内机（接听端）—— 注册模式（经 miniSIPServer）
rem 前提：本机 miniSIPServer 在 5060（域任意，账号 1001/1002 免鉴权）。
rem 响铃后用屏幕上的"接听"按钮手动接；首页点 SIP 状态字可进拨号页呼出。
rem 端口：SIP 5062 / RTP 6062（+2 为视频口）
rem ============================================================
setlocal
cd /d "%~dp0"

set SIP_INI=%~dp0bin\sip.ini
set SIP_SERVER=127.0.0.1
set SIP_PORT=5060
set SIP_LOCAL_PORT=5062
set SIP_USER=1001

echo [实例A 室内机] 注册到 127.0.0.1:5060（账号 1001），等呼叫...
bin\main.exe
endlocal
