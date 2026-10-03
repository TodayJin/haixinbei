@echo off
rem ASCII only in this file: see the note in the real-car launcher .bat.
rem Simulated car: no hardware needed, use this to poke at the UI.
setlocal
chcp 65001 >nul
set "PSEXE=pwsh"
where pwsh >nul 2>nul || set "PSEXE=powershell"
"%PSEXE%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" -Sim -Pause %*
if errorlevel 1 (
  echo.
  echo [launcher] failed, exit code %ERRORLEVEL%
  pause
)
endlocal
