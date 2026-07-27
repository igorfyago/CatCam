@echo off
REM Elevated one-shot: swap the running tray for a freshly built one.
REM Host is NOT touched: the new tray adopts the running CatCamHost.exe.
cd /d %~dp0
taskkill /F /IM CatCamTray.exe >nul 2>&1
timeout /t 1 /nobreak >nul
call buildtray.bat > traydeploy.log 2>&1
if errorlevel 1 exit /b 1
start "" CatCamTray.exe
