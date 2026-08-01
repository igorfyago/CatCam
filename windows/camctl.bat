@echo off
REM Drive the tablet CatCam app over adb: camctl.bat start|stop|flip
REM Sends intent ACTIONS (UI_START/UI_STOP/UI_FLIP) to MainActivity, which
REM routes them through the SAME handlers as the on-screen controls, so the
REM Android UI stays in sync by construction. Replaces the old hardcoded
REM screen-coordinate taps, which broke whenever the layout moved (and
REM missed entirely during cold starts). singleTop: a running activity gets
REM the action via onNewIntent; a cold one gets it in onCreate. Idempotent:
REM start-while-streaming and stop-while-idle are no-ops.
cd /d %~dp0
call "%~dp0catcam-env.bat"
set ADB=%CATCAM_ADB%
set SER=%SERFLAG%
"%ADB%" %SER% shell input keyevent KEYCODE_WAKEUP || exit /b 1
"%ADB%" %SER% shell wm dismiss-keyguard >nul 2>&1
set ACTION=
if "%1"=="start" set ACTION=com.catcam.app.UI_START
if "%1"=="stop"  set ACTION=com.catcam.app.UI_STOP
if "%1"=="flip"  set ACTION=com.catcam.app.UI_FLIP
if "%ACTION%"=="" (
    "%ADB%" %SER% shell am start -n com.catcam.app/.MainActivity
    exit /b %errorlevel%
)
"%ADB%" %SER% shell am start -n com.catcam.app/.MainActivity -a %ACTION% || exit /b 1
