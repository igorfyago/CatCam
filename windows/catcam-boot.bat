@echo off
REM CatCam logon bootstrap (run by the "CatCam" scheduled task, elevated).
REM Reboot resilience (HANDOFF lesson 33): waits for the tablet over adb,
REM re-establishes the port forward (lost on every reboot), starts the tray
REM (which starts the host, which re-registers the session-scoped camera).
cd /d %~dp0
call "%~dp0catcam-env.bat"
set ADB=%CATCAM_ADB%
set SER=%SERFLAG%

REM Wait up to ~90s for the tablet (USB enumeration after boot takes a while).
set /a tries=0
:wait
"%ADB%" %SER% get-state >nul 2>&1 && goto ready
set /a tries+=1
if %tries% geq 30 goto ready
REM ping delay, not timeout.exe (needs console stdin; keep bats context-proof)
ping -n 4 127.0.0.1 >nul
goto wait

:ready
"%ADB%" %SER% forward tcp:9000 tcp:9000 >nul 2>&1
start "" CatCamTray.exe
