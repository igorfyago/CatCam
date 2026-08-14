# CatCam diagnostics: gathers what a GitHub issue needs into one zip on the
# Desktop. Read-only, local only, nothing is sent anywhere; the user attaches
# the zip themselves. Local IPs and the PC name do appear in it (they are
# what connectivity problems are made of), so say so in the output.
$ErrorActionPreference = 'SilentlyContinue'
$app = Split-Path -Parent $MyInvocation.MyCommand.Path
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$work = Join-Path $env:TEMP "catcam-diag-$stamp"
New-Item -ItemType Directory -Force $work | Out-Null

$report = @()
$report += "CatCam diagnostics $stamp"
$report += "OS: $([Environment]::OSVersion.VersionString) build $((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuild).$((Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').UBR)"
$report += ""
$report += "== Files in $app =="
Get-ChildItem $app -File | ForEach-Object { $report += "{0}  {1,10}  {2}" -f $_.LastWriteTime.ToString('yyyy-MM-dd HH:mm'), $_.Length, $_.Name }
$report += ""
$report += "== Processes =="
foreach ($p in 'CatCamTray','CatCamHost','adb') {
    $proc = Get-Process $p -ErrorAction SilentlyContinue
    $report += "{0}: {1}" -f $p, $(if ($proc) { "running (pid $($proc.Id -join ','))" } else { "not running" })
}
$report += ""
$report += "== Startup task =="
$report += (schtasks /query /tn CatCam /v /fo list 2>&1 | Out-String)
$report += "== Sound devices =="
Get-CimInstance Win32_SoundDevice | ForEach-Object { $report += $_.Name }
$report += ""
$report += "== IPv4 addresses (local, for same-network checks) =="
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
    ForEach-Object { $report += "{0}  {1}" -f $_.IPAddress, $_.InterfaceAlias }
$report += ""
$report += "== Listening sockets of CatCamHost =="
$hostProc = Get-Process CatCamHost -ErrorAction SilentlyContinue
if ($hostProc) {
    Get-NetTCPConnection -OwningProcess $hostProc.Id -State Listen | ForEach-Object { $report += "TCP $($_.LocalAddress):$($_.LocalPort)" }
    Get-NetUDPEndpoint  -OwningProcess $hostProc.Id | ForEach-Object { $report += "UDP $($_.LocalAddress):$($_.LocalPort)" }
} else { $report += "(host not running)" }
$report += ""
$report += "== Firewall rules mentioning CatCam =="
Get-NetFirewallRule | Where-Object DisplayName -match 'CatCam' |
    ForEach-Object { $report += "{0}: {1}, {2}, {3}" -f $_.DisplayName, $_.Direction, $_.Action, $_.Enabled }

$report -join "`r`n" | Set-Content (Join-Path $work 'report.txt')
foreach ($log in @((Join-Path $app 'tray.log'), (Join-Path $app 'host.log'), 'C:\CatCam.log')) {
    if (Test-Path $log) { Copy-Item $log $work }
}

$zip = Join-Path ([Environment]::GetFolderPath('Desktop')) "CatCam-diagnostics-$stamp.zip"
Compress-Archive -Path (Join-Path $work '*') -DestinationPath $zip -Force
Remove-Item -Recurse -Force $work

Write-Host ""
Write-Host "Diagnostics written to: $zip"
Write-Host "It contains CatCam logs, device and network basics (local IPs, PC name)."
Write-Host "Review it if you like, then attach it to an issue at:"
Write-Host "  https://github.com/igorfyago/CatCam/issues"
Write-Host ""
Read-Host "Enter to close"
