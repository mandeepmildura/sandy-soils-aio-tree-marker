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
echo  Make sure AUTOSTEER IS OFF and every Chrome window is closed.
echo.
pause
echo.
echo  1. Checking the AiO at %BOARD% ...
ping -n 2 -w 1000 %BOARD% | findstr "TTL=" >nul
if errorlevel 1 (
  echo     The AiO does not answer. Check its power and network cable,
  echo     switch it off and on, wait 20 seconds, then run this again.
  goto :done
)
echo     OK
echo  2. Downloading the latest firmware from GitHub ...
curl.exe -fsSL -o "%HEX%" "%URL%/aio_v4_tree.hex"
if errorlevel 1 (
  echo     Download failed. Is the Surface on the internet ^(phone hotspot^)?
  goto :done
)
for /f %%v in ('curl.exe -fsSL "%URL%/version.txt"') do set VER=%%v
echo     Got version %VER%
echo  3. Sending it to the AiO ...
set TRY=1
:send
curl.exe -sS -m 90 --data-binary "@%HEX%" http://%BOARD%/update
set RC=%errorlevel%
echo.
if "%RC%"=="0" goto :check
echo     No answer from the AiO ^(curl error %RC%^).
if not "%TRY%"=="1" goto :check
echo     Trying once more in 15 seconds...
set TRY=2
ping -n 16 127.0.0.1 >nul
goto :send
:check
echo  4. Waiting 20 seconds, then asking the AiO which version it runs ...
ping -n 21 127.0.0.1 >nul
curl.exe -s -m 10 http://%BOARD%/status > "%TEMP%\aio_status.txt"
findstr /c:"\"ver\":\"%VER%\"" "%TEMP%\aio_status.txt" >nul
if errorlevel 1 goto :notyet
echo.
echo     DONE: the AiO now runs v%VER%. The page at %BOARD% is safe to open.
goto :done
:notyet
echo.
echo     The AiO did not report v%VER%. It answered:
type "%TEMP%\aio_status.txt"
echo.
echo     ^(blank means no answer^). Switch the AiO off and on, wait 20 seconds,
echo     close every Chrome window, and run this file again.
echo     If it fails again, send Mandeep a photo of this whole window.
:done
echo.
pause
