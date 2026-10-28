@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
rem KWS test launcher (cmd / double-click friendly). ASCII only on purpose.
set "HERE=%~dp0"
set "MD=%HERE%..\..\..\.tools\kws"
set "KW=%MD%\keywords_xixi.txt"

set "PY="
for %%P in ("C:\Python314\python.exe") do (
  if exist %%~P (
    %%~P -c "import sherpa_onnx" >nul 2>&1 && set "PY=%%~P"
  )
)
if not defined PY (
  echo ERROR: no Python with sherpa-onnx found.
  echo Install: C:\Python314\python.exe -m pip install sherpa-onnx sherpa-onnx-core
  exit /b 1
)

set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
"%PY%" -u "%HERE%kws_test.py" --model-dir "%MD%" --keywords "%KW%" %*
