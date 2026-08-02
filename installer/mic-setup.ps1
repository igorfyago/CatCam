# Microphone path: ensure VB-Audio Virtual Cable is installed. Proprietary
# donationware, deliberately NOT bundled (redistribution needs a commercial
# license): fetched from the official site at install time.
#
# Silent-first: VBCABLE_Setup_x64.exe accepts -i (install) -h (hidden),
# undocumented but VM-verified (fresh Server 2025: exit 0 and the
# "VB-Audio Virtual Cable" sound device present, no GUI). If the silent
# attempt does not stick, fall back to VB-Audio's interactive installer,
# but only when a user is present to click it (-AllowGui).
param([switch]$AllowGui)
$cable = Get-CimInstance Win32_SoundDevice -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -match 'VB-Audio' }
if ($cable) { Write-Host "VB-Cable already installed."; exit 0 }
$zip = Join-Path $env:TEMP 'vbcable.zip'
$dst = Join-Path $env:TEMP 'vbcable'
$got = $false
foreach ($u in @('https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip',
                 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack43.zip')) {
    try { Invoke-WebRequest $u -OutFile $zip -UseBasicParsing; $got = $true; break } catch {}
}
if (-not $got) {
    if ($AllowGui) { Start-Process 'https://vb-audio.com/Cable/' }
    Write-Host "VB-Cable download failed: install manually from vb-audio.com/Cable."
    exit 0
}
Expand-Archive $zip $dst -Force
$setup = Get-ChildItem $dst -Recurse -Filter 'VBCABLE_Setup_x64.exe' | Select-Object -First 1
if (-not $setup) { Write-Host "VBCABLE_Setup_x64.exe not found in the pack."; exit 0 }
Start-Process $setup.FullName -ArgumentList '-i','-h' -Wait
Start-Sleep -Seconds 6
$dev = Get-CimInstance Win32_SoundDevice -ErrorAction SilentlyContinue |
       Where-Object { $_.Name -match 'VB-Audio' }
if ($dev) { Write-Host "VB-Cable installed silently."; exit 0 }
if ($AllowGui) {
    Write-Host "Silent install did not stick: opening VB-Audio's installer, click 'Install Driver'."
    Start-Process $setup.FullName -Wait
} else {
    Write-Host "Silent install did not stick; run mic-setup.ps1 -AllowGui interactively."
}
