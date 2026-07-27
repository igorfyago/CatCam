@echo off
REM Re-establish the stream's adb port forward. Invoked by the tray's
REM self-heal (frames stalled -> forward probably dead) and safe to run by
REM hand: idempotent when the forward exists, harmless with no device.
cd /d %~dp0
call "%~dp0catcam-env.bat"
"%CATCAM_ADB%" %SERFLAG% forward tcp:9000 tcp:9000
