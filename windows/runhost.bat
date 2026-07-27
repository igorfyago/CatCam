@echo off
REM Manual/debug host restart WITHOUT the tray. The tray is killed first:
REM its watchdog would race this with its own respawn, and a duplicate host
REM re-registers the camera (HANDOFF lesson 16). Optional arg = a direct
REM tablet IP (the host is a plain TCP client and still accepts one; debug
REM only, see lesson 37). Normal operation: the tray owns the host.
cd /d %~dp0
taskkill /F /IM CatCamTray.exe 2>nul
taskkill /F /IM CatCamHost.exe 2>nul
CatCamHost.exe %1 > host.log 2>&1
