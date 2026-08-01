# Microphone path: ensure VB-Audio Virtual Cable is installed. Proprietary
# donationware, deliberately NOT bundled (redistribution needs a commercial
# license): fetched from the official site, the user accepts VB-Audio's own
# installer. Invoked by CatCamSetup when the microphone task is selected.
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
    Start-Process 'https://vb-audio.com/Cable/'
    Write-Host "Download failed: install manually from the page that opened."
    exit 0
}
Expand-Archive $zip $dst -Force
$setup = Get-ChildItem $dst -Recurse -Filter 'VBCABLE_Setup_x64.exe' | Select-Object -First 1
if ($setup) { Start-Process $setup.FullName -Wait }
