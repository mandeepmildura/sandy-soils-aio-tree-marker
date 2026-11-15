@echo off
rem Update the AiO tree marker firmware from the Surface, no web browser needed.
rem Double-click it. Needs internet (phone hotspot) and the AiO on the network cable.
rem Autosteer must be OFF.
setlocal
set BOARD=192.168.5.126
set HEX=%TEMP%\aio_v4_tree.hex
set URL=https://raw.githubusercontent.com/mandeepmildura/sandy-soils-aio-tree-marker/ota
echo.
echo  AiO tree marker update
echo  ----------------------
echo  Make sure AUTOSTEER IS OFF. Close any browser tab showing %BOARD%.
echo.
pause
echo.
echo  1. Checking the AiO at %BOARD% ...
ping -n 2 -w 1000 %BOARD% | findstr "TTL=" >nul
if errorlevel 1 (
  echo     The AiO does not answer. Check its power and network cable,
  echo     switch it off and on, wait 20 seconds, then run this again.
  goto :end
)
echo     OK
echo  2. Downloading the latest firmware from GitHub ...
curl.exe -fsSL -o "%HEX%" "%URL%/aio_v4_tree.hex" || (
  echo     Download failed. Is the Surface on the internet ^(phone hotspot^)?
  goto :end
)
for /f %%v in ('curl.exe -fsSL "%URL%/version.txt"') do set VER=%%v
echo     Got version %VER%
echo  3. Sending it to the AiO (about 5 seconds) ...
curl.exe -s -m 120 --data-binary "@%HEX%" http://%BOARD%/update
echo.
echo.
echo  If it says "OK - verified", the AiO restarts with v%VER% in about 10 seconds.
echo  Any other message: nothing was changed. Send Mandeep a photo of this window.
:end
echo.
pause
