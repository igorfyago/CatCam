@echo off
REM Elevated one-shot: swap BOTH the host and the tray for freshly built
REM ones. Unlike traydeploy.bat this must stop the host too, because the
REM host binary itself changed. The DLL is untouched, so the Frame Server
REM is left alone (lesson 16: never re-register mid-session).
cd /d %~dp0
echo === stopping tray (watchdog first, so it cannot respawn the host) ===
taskkill /F /IM CatCamTray.exe >nul 2>&1
taskkill /F /IM CatCamHost.exe >nul 2>&1
timeout /t 2 /nobreak >nul

echo === building host ===
call buildhost.bat > deployfix.log 2>&1
if errorlevel 1 (echo HOST BUILD FAILED - see deployfix.log, old binaries intact & pause & exit /b 1)

echo === building tray ===
call buildtray.bat >> deployfix.log 2>&1
if errorlevel 1 (echo TRAY BUILD FAILED - see deployfix.log, old binaries intact & pause & exit /b 1)

echo === archiving the runaway logs ===
if exist host.log move /y host.log host.log.prefix >nul 2>&1
if exist tray.log move /y tray.log tray.log.prefix >nul 2>&1

echo === restarting via the scheduled task (elevated, re-adds adb forward) ===
schtasks /run /tn CatCam
timeout /t 3 /nobreak >nul
echo === DEPLOYFIX DONE ===
timeout /t 4 /nobreak >nul
