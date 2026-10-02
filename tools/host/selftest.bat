@echo off
rem ============================================================
rem  Run all selftests (no hardware needed).
rem  MUST stay CRLF + ASCII-only - see run_host.bat for why.
rem ============================================================
setlocal
set PYTHONUTF8=1
cd /d "%~dp0"

set "PYEXE="
where python >nul 2>nul && set "PYEXE=python"
if not defined PYEXE where py >nul 2>nul && set "PYEXE=py -3"
if not defined PYEXE (
  echo [ERROR] Python not found in PATH.
  pause
  exit /b 1
)

%PYEXE% tests\run_all.py
echo.
pause
exit /b %errorlevel%
