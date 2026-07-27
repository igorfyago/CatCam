@echo off
REM Elevated one-shot: rebuild DLL + host + tray and restart the whole chain.
REM Follows HANDOFF restart procedure; ONE FrameServer cycle (lesson 16).
cd /d %~dp0
echo === stopping === > fullredeploy.log
taskkill /F /IM CatCamTray.exe >nul 2>&1
taskkill /F /IM CatCamHost.exe >nul 2>&1
net stop FrameServer /y >> fullredeploy.log 2>&1
call build.bat >> fullredeploy.log 2>&1
if errorlevel 1 (
  echo BUILD FAILED - rolling back to old binaries >> fullredeploy.log
  net start FrameServer >> fullredeploy.log 2>&1
  start "" CatCamTray.exe
  exit /b 1
)
regsvr32 /s CatCamSource.dll
icacls CatCamSource.dll /grant "NT AUTHORITY\LOCAL SERVICE":RX >> fullredeploy.log 2>&1
net start FrameServer >> fullredeploy.log 2>&1
start "" CatCamTray.exe
echo === done === >> fullredeploy.log
