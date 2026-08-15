# Microphone path: ensure VB-Audio Virtual Cable is installed. Proprietary
# donationware by Vincent Burel; its EULA allows copying/diffusing the
# UNMODIFIED pack "as is", so CatCam ships the official zip untouched at
# payload\VBCABLE_Driver_Pack45.zip and installs from that local copy. The
# official-site download is only a fallback for checkouts without the
# payload. Both paths are pinned to the pack's SHA256: a driver installer
# is not something to run on a hash mismatch.
# Per the EULA, distribution must mention:
#   1- The origin of VB-CABLE: www.vb-cable.com
#   2- VB-CABLE is a donationware, all participations are welcome.
#
# Silent-first: VBCABLE_Setup_x64.exe accepts -i (install) -h (hidden),
# undocumented but VM-verified (fresh Server 2025: exit 0 and the
# "VB-Audio Virtual Cable" sound device present, no GUI). If the silent
# attempt does not stick, fall back to VB-Audio's interactive installer,
# but only when a user is present to click it (-AllowGui).
param([switch]$AllowGui)

$PackSha256 = 'B950E39F01AF1D04EA623C8F6D8EB9B6EA5C477C637295FABF20631C85116BFB'

# Exact product: 'VB-Audio' alone also matches Voicemeeter, Hi-Fi Cable and
# Cable A/B, on which VB-Cable itself is absent and the mic would never exist.
$cable = Get-CimInstance Win32_SoundDevice -ErrorAction SilentlyContinue |
         Where-Object { $_.Name -match 'VB-Audio Virtual Cable' }
# Ownership record for CatCamAudio.exe: a cable the user already had is not
# CatCam's to rename or trim; one CatCam installs is. Written once.
$own = 'HKLM:\SOFTWARE\CatCam'
function Set-CableOwner([int]$ours) {
    if (-not (Test-Path $own)) { New-Item $own -Force | Out-Null }
    if ($null -eq (Get-ItemProperty $own -Name CableInstalledByCatCam -ErrorAction SilentlyContinue)) {
        New-ItemProperty $own -Name CableInstalledByCatCam -Value $ours -PropertyType DWord | Out-Null
    }
}
if ($cable) { Write-Host "VB-Cable already installed."; Set-CableOwner 0; exit 0 }

Write-Host "Microphone endpoint: VB-CABLE by VB-Audio (www.vb-cable.com)."
Write-Host "VB-CABLE is a donationware, all participations are welcome."

$zip = $null
$local = Join-Path $PSScriptRoot 'payload\VBCABLE_Driver_Pack45.zip'
if (Test-Path $local) {
    if ((Get-FileHash $local -Algorithm SHA256).Hash -eq $PackSha256) { $zip = $local }
    else { Write-Host "Bundled VB-Cable pack failed its checksum; trying the official download." }
}
if (-not $zip) {
    $dl = Join-Path $env:TEMP 'vbcable.zip'
    try {
        Invoke-WebRequest 'https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip' -OutFile $dl -UseBasicParsing
        if ((Get-FileHash $dl -Algorithm SHA256).Hash -eq $PackSha256) { $zip = $dl }
        else { Write-Host "Downloaded VB-Cable pack failed its checksum; not running it." }
    } catch {}
}
if (-not $zip) {
    if ($AllowGui) { Start-Process 'https://vb-audio.com/Cable/' }
    Write-Host "No usable VB-Cable pack: install manually from vb-audio.com/Cable."
    exit 0
}

$dst = Join-Path $env:TEMP 'vbcable'
Expand-Archive $zip $dst -Force
$setup = Get-ChildItem $dst -Recurse -Filter 'VBCABLE_Setup_x64.exe' | Select-Object -First 1
if (-not $setup) { Write-Host "VBCABLE_Setup_x64.exe not found in the pack."; exit 0 }
Start-Process $setup.FullName -ArgumentList '-i','-h' -Wait
Start-Sleep -Seconds 6
$dev = Get-CimInstance Win32_SoundDevice -ErrorAction SilentlyContinue |
       Where-Object { $_.Name -match 'VB-Audio Virtual Cable' }
if ($dev) { Write-Host "VB-Cable installed silently."; Set-CableOwner 1; exit 0 }
if ($AllowGui) {
    Write-Host "Silent install did not stick: opening VB-Audio's installer, click 'Install Driver'."
    Start-Process $setup.FullName -Wait
    Set-CableOwner 1
} else {
    Write-Host "Silent install did not stick; run mic-setup.ps1 -AllowGui interactively."
}
