@echo off
REM Shared settings loader for the operations scripts. Deliberately NO
REM setlocal: callers receive the variables. Machine-local overrides live
REM in catcam.env.bat next to this file (git-ignored; the installer writes
REM it, or copy catcam.env.bat.example). Without one, portable defaults:
REM   CATCAM_ADB     %USERPROFILE%\android-sdk\platform-tools\adb.exe
REM   CATCAM_SERIAL  empty -> SERFLAG=-d (the USB device, any serial; a
REM                  stale adb-WiFi entry of the same tablet otherwise
REM                  breaks bare adb with "more than one device")
REM   set a serial   -> SERFLAG=-s <serial> (only needed with SEVERAL
REM                  USB devices attached)
if exist "%~dp0catcam.env.bat" call "%~dp0catcam.env.bat"
if not defined CATCAM_ADB set CATCAM_ADB=%USERPROFILE%\android-sdk\platform-tools\adb.exe
if defined CATCAM_SERIAL (set SERFLAG=-s %CATCAM_SERIAL%) else (set SERFLAG=-d)
