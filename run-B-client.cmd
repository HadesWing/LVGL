@echo off
rem ============================================================
rem 实例 B：客户端（呼叫端）
rem 双击运行，启动后自动呼叫实例 A（127.0.0.1:5062）。
rem 想改呼叫目标：把 AUTOCALL 改成 sip:1001@对方IP:5062（局域网直接填对方 IP）。
rem 端口：SIP 7062 / RTP 8062 —— 和实例 A 整块错开，绝不能相同
rem ============================================================
setlocal
cd /d "%~dp0"

set SIP_LOCAL_PORT=7062
set SIP_USER=2002
set SIP_NO_REGISTER=1
set SIP_AUTOCALL=sip:1001@127.0.0.1:5062

echo [实例B 客户端] 正在呼叫 127.0.0.1:5062 ...
bin\main.exe
endlocal
