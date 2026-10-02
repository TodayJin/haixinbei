@echo off
rem ============================================================
rem  All-terrain robot - host GUI launcher
rem
rem  NOTE: this file MUST stay CRLF + ASCII-only. cmd.exe mis-parses
rem  LF-only or UTF-8-Chinese .bat files (that is why the previous
rem  version printed "not recognized as an internal or external
rem  command"). Keep all messages in English here.
rem ============================================================
setlocal
set PYTHONUTF8=1
cd /d "%~dp0"

set "PYEXE="
where python >nul 2>nul && set "PYEXE=python"
if not defined PYEXE where py >nul 2>nul && set "PYEXE=py -3"
if not defined PYEXE (
  echo.
  echo [ERROR] Python not found in PATH.
  echo         Install Python 3 first, then run this again.
  echo.
  pause
  exit /b 1
)

echo Starting host GUI with the built-in simulator ^(no hardware needed^)...
%PYEXE% main.py --sim %*
if errorlevel 1 (
  echo.
  echo [ERROR] Failed to start. Things to try:
  echo     pip install PySide6 pyqtgraph     ^(modern GUI, recommended^)
  echo     python main.py --ui tk             ^(fallback GUI, no extra deps^)
  echo     pip install pyserial               ^(only needed for a real COM port^)
  echo.
  pause
)
