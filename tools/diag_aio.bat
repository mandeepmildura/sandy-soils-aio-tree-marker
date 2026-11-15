@echo off
rem AiO health check from the Surface. Double-click it, then send Mandeep a photo of the window.
setlocal
set BOARD=192.168.5.126
echo.
echo  AiO health check
echo  ----------------
echo  Close every Chrome window first.
echo.
pause
echo.
echo  A. Network (ping) ...
ping -n 2 -w 1000 %BOARD% | findstr "TTL=" >nul
if errorlevel 1 (echo     NO ANSWER) else (echo     OK)
echo  B. Main program (tree marker message on UDP 8890) ...
powershell -NoProfile -Command "$u=New-Object Net.Sockets.UdpClient 8891; $u.Client.ReceiveTimeout=3000; $b=[Text.Encoding]::ASCII.GetBytes('TMQ'); [void]$u.Send($b,$b.Length,'%BOARD%',8890); try { $ep=New-Object Net.IPEndPoint([Net.IPAddress]::Any,0); $r=$u.Receive([ref]$ep); if ($r.Length -ge 49) { $g=[BitConverter]::ToUInt32($r,45); $s=[BitConverter]::ToUInt32($r,37) } else { $g='?'; $s='?' }; '    RUNNING (fix quality ' + $r[3] + ', GPS bytes ' + $s + ', GGA ' + $g + ')' } catch { '    NO ANSWER (main program stuck, or older firmware)' } finally { $u.Close() }"
echo  C. Web server (/status) ...
curl.exe -sS -m 15 http://%BOARD%/status
echo.
if errorlevel 1 echo     NO ANSWER from the web server
echo.
echo  Send Mandeep a photo of this whole window.
echo.
pause
