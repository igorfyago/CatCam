@echo off
REM Drive the tablet CatCam app UI over adb: camctl.bat start|stop|flip
REM Taps the REAL app buttons so the Android UI stays in sync (its labels
REM re-read service state every 500ms). Wake + foreground first so taps
REM always land on the CatCam activity, never on whatever else is open.
REM Button coords for SM-T220 portrait, measured via uiautomator (HANDOFF).
cd /d %~dp0
call "%~dp0catcam-env.bat"
set ADB=%CATCAM_ADB%
set SER=%SERFLAG%
"%ADB%" %SER% shell input keyevent KEYCODE_WAKEUP || exit /b 1
"%ADB%" %SER% shell wm dismiss-keyguard >nul 2>&1
"%ADB%" %SER% shell am start -n com.catcam.app/.MainActivity || exit /b 1
REM ping delay, not timeout.exe: timeout dies with "input redirection is
REM not supported" when spawned without console stdin.
ping -n 3 127.0.0.1 >nul
if "%1"=="start" "%ADB%" %SER% shell input tap 229 1217
if "%1"=="stop"  "%ADB%" %SER% shell input tap 400 1217
if "%1"=="flip"  "%ADB%" %SER% shell input tap 571 1217
