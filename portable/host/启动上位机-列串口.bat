@echo off
rem ASCII only in this file: see the note in the real-car launcher .bat.
rem List serial ports only (diagnostic: is the car plugged in, and on which COM).
setlocal
chcp 65001 >nul
set "PSEXE=pwsh"
where pwsh >nul 2>nul || set "PSEXE=powershell"
"%PSEXE%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" -ListPorts -Pause %*
if errorlevel 1 (
  echo.
  echo [launcher] failed, exit code %ERRORLEVEL%
  pause
)
endlocal
