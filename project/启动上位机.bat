@echo off
rem ASCII only in this file: chcp 65001 plus any non-ASCII byte in the same .bat makes
rem cmd.exe re-read the file at shifted offsets and run comment fragments as commands.
rem Repo-root shortcut -> tools\console\launch.ps1 (real car by default).
rem Extra switches pass through to launch.ps1, e.g. -Sim / -ListPorts / -HttpPort 8791.
setlocal
chcp 65001 >nul
set "PSEXE=pwsh"
where pwsh >nul 2>nul || set "PSEXE=powershell"
"%PSEXE%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\console\launch.ps1" -Pause %*
if errorlevel 1 (
  echo.
  echo [launcher] failed, exit code %ERRORLEVEL%
  pause
)
endlocal
