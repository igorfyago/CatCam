#Requires -RunAsAdministrator
<#
CatCam uninstaller. Removes the logon task, stops the processes and
unregisters the COM media source. Leaves: the repo itself, logs, and
VB-Cable (uninstall that from its own uninstaller in Program Files\VB\
if you no longer want it).
#>
$ErrorActionPreference = 'SilentlyContinue'
$root = Split-Path $PSScriptRoot -Parent
$win  = Join-Path $root 'windows'

schtasks /delete /tn CatCam /f | Out-Null
taskkill /F /IM CatCamTray.exe  2>$null | Out-Null
taskkill /F /IM CatCamHost.exe  2>$null | Out-Null

# The Frame Server caches the DLL per session; cycle it so the file is
# released before unregistering.
net stop FrameServer /y | Out-Null
& regsvr32 /u /s (Join-Path $win 'CatCamSource.dll')
net start FrameServer | Out-Null

Write-Host "CatCam removed: task deleted, processes stopped, COM source unregistered."
Write-Host "VB-Cable (if installed) has its own uninstaller and was left in place."
