@echo off
rem AiO -> HiveMQ relay. Double-click to run.
rem Needs the companion relay_aio.ps1 in the same folder.
rem First run asks for the broker host, user and password; they are saved
rem per-user with Windows DPAPI under %APPDATA%\sandy-soils-aio\.
setlocal
set PS1=%~dp0relay_aio.ps1
if not exist "%PS1%" (
  echo relay_aio.ps1 is missing. Download it next to this .bat and try again.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
pause
