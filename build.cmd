@echo off
rem ===========================================================================
rem  PATH-free wrapper for "make" (ASCII only - cmd.exe parses batch in ANSI).
rem
rem    build.cmd             build + run
rem    build.cmd build       build only
rem    build.cmd run         run only
rem    build.cmd stats       print FPS / memory / loop cost
rem    build.cmd stress      30s stress test (leak / fragmentation)
rem    build.cmd mem64       64KB memory pool build
rem    build.cmd mem SIZE=40960
rem    build.cmd debug       build with LVGL asserts
rem    build.cmd help        help
rem
rem  Why: make.exe lives in C:\msys64\mingw64\bin and only new terminals pick
rem  up the updated PATH. This script calls MSYS2 bash directly.
rem ===========================================================================
chcp 65001 >nul 2>&1
setlocal
set "BASH=C:\msys64\usr\bin\bash.exe"
if not exist "%BASH%" (
    echo [ERROR] %BASH% not found. Install MSYS2 to C:\msys64 first.
    exit /b 1
)
if "%~1"=="" (
    "%BASH%" "%~dp0scripts\dev.sh" build
    if errorlevel 1 exit /b 1
    "%BASH%" "%~dp0scripts\dev.sh" run
) else (
    "%BASH%" "%~dp0scripts\dev.sh" %*
)
