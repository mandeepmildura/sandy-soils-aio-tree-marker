@echo off
rem AiO relay for the cab Surface. Double-click to run; keep the window open while marking.
rem Logs every AiO reading to Documents\AiO logs, shares it on port 8126 (phone hotspot),
rem and sends it to the Sandy Soil MQTT broker for data collection. Needs relay_aio.ps1
rem in the same folder. No questions: the broker password is set once from the phone page.
setlocal
set PS1=%~dp0relay_aio.ps1
if not exist "%PS1%" (
  echo relay_aio.ps1 is missing. Download it next to this .bat and try again.
  pause
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" %*
pause
