@echo off
rem ============================================================
rem 实例 B：客户端（呼叫端）—— 注册模式（经 miniSIPServer）
rem 与实例 A 同一服务器，账号 1002；启动后自动呼叫 1001。
rem 不想自动呼叫：删掉 SIP_AUTOCALL 一行，改用首页拨号页手动输房号。
rem 端口：SIP 7062 / RTP 8062 —— 和实例 A 整块错开，绝不能相同
rem ============================================================
setlocal
cd /d "%~dp0"

set SIP_INI=%~dp0bin\sip.ini
set SIP_SERVER=127.0.0.1
set SIP_PORT=5060
set SIP_LOCAL_PORT=7062
set SIP_USER=2002
set SIP_AUTOCALL=1001

echo [实例B 客户端] 注册 1002，正在呼叫房号 1001 ...
bin\main.exe
endlocal
