@echo off
rem ASCII only in this file: chcp 65001 plus non-ASCII text in the same .bat makes
rem cmd.exe re-read the file at shifted offsets and run comment fragments.
rem All Chinese messages come from launch.ps1 instead.
rem Real car: auto-detect the serial port, 230400 (= DL_BAUD in firmware board.c).
setlocal
chcp 65001 >nul
set "PSEXE=pwsh"
where pwsh >nul 2>nul || set "PSEXE=powershell"
"%PSEXE%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" -Pause %*
if errorlevel 1 (
  echo.
  echo [launcher] failed, exit code %ERRORLEVEL%
  pause
)
endlocal
