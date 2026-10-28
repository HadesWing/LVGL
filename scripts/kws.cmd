@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
rem KWS test launcher (cmd / double-click friendly). ASCII only on purpose.
rem Prefers conda env kws; falls back to other pythons that have sherpa-onnx.
rem NOTE: this file must keep CRLF line endings, otherwise the multi-line
rem       for/if blocks get mis-parsed. .gitattributes pins eol=crlf for *.cmd.

set "HERE=%~dp0"
set "MD=%HERE%..\..\..\.tools\kws"
set "KW=%MD%\keywords_xixi.txt"

set "ENVNAME=kws"
if not "%KWS_ENV%"=="" set "ENVNAME=%KWS_ENV%"

set "PY="
for %%P in (
  "D:\Program Files\anaconda3\envs\%ENVNAME%\python.exe"
  "C:\ProgramData\anaconda3\envs\%ENVNAME%\python.exe"
  "C:\ProgramData\miniconda3\envs\%ENVNAME%\python.exe"
  "%USERPROFILE%\anaconda3\envs\%ENVNAME%\python.exe"
  "%USERPROFILE%\miniconda3\envs\%ENVNAME%\python.exe"
  "C:\Python314\python.exe"
) do (
  if not defined PY if exist %%~P (
    %%P -c "import sherpa_onnx" >nul 2>&1 && set "PY=%%~P"
  )
)

if not defined PY (
  echo ERROR: no Python with sherpa-onnx found.
  echo Fix: conda activate %ENVNAME% ^&^& python -m pip install sherpa-onnx sherpa-onnx-core
  exit /b 1
)

set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"

rem --mic mode needs ffmpeg for DirectShow capture; add MSYS2 if missing
if exist "C:\msys64\mingw64\bin\ffmpeg.exe" set "PATH=%PATH%;C:\msys64\mingw64\bin"

"%PY%" -u "%HERE%kws_test.py" --model-dir "%MD%" --keywords "%KW%" %*
