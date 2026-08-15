; CatCam one-click installer (Inno Setup 6).
; Replaces the elevated-PowerShell ceremony of install.ps1 with a wizard:
; copies the runtime to Program Files (which LOCAL SERVICE can read, so the
; old icacls grant is unnecessary), registers the COM media source, creates
; the elevated logon task, sets up the microphone path as standard (VB-Audio
; Virtual Cable: the official UNMODIFIED pack ships in payload\, which their
; EULA permits "as is" with attribution; mic-setup.ps1 carries the required
; mentions and a pinned SHA256) and optionally fetches Google platform-tools
; for USB cable mode. Wi-Fi mode needs no cable and no adb.
; Build: iscc CatCamSetup.iss   (output: Output\CatCamSetup.exe)

[Setup]
AppId={{7C1FA2E5-4B3D-4A80-9E6F-CATCAM000001}
AppName=CatCam
AppVersion=1.2.0
AppPublisher=Igor Yago
AppPublisherURL=https://catcam.app
AppSupportURL=https://github.com/igorfyago/CatCam
DefaultDirName={autopf}\CatCam
; 64-bit binaries: without this Inno runs in 32-bit install mode, lands in
; Program Files (x86) and {sys} resolves to SysWOW64 (found by the VM test).
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
DisableProgramGroupPage=yes
PrivilegesRequired=admin
OutputBaseFilename=CatCamSetup
SetupIconFile=catcam.ico
UninstallDisplayIcon={app}\CatCamTray.exe
WizardStyle=modern
Compression=lzma2
SolidCompression=yes

[Files]
Source: "..\windows\CatCamHost.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\CatCamSource.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\CatCamTray.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\CatCamAudio.exe";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\catcam-boot.bat";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\camctl.bat";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\adbfwd.bat";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\catcam-env.bat";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\catcam64.png";     DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\probe.py";         DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";                  DestDir: "{app}"; Flags: ignoreversion
Source: "..\NOTICE";                   DestDir: "{app}"; Flags: ignoreversion
Source: "mic-setup.ps1";               DestDir: "{app}"; Flags: ignoreversion
; The official VB-CABLE pack, byte-identical to VB-Audio's download ("as is"
; per their EULA). mic-setup.ps1 installs from this copy; no network needed.
Source: "payload\VBCABLE_Driver_Pack45.zip"; DestDir: "{app}\payload"; Flags: ignoreversion
Source: "usb-setup.ps1";               DestDir: "{app}"; Flags: ignoreversion
Source: "task-setup.ps1";              DestDir: "{app}"; Flags: ignoreversion
Source: "catcam-diag.ps1";             DestDir: "{app}"; Flags: ignoreversion
Source: "catcam.ico";                  DestDir: "{app}"; Flags: ignoreversion

[Registry]
; Written by CatCamAudio.exe (cable pin, endpoint originals, ownership) and
; the host (stream dims). Deleted at uninstall AFTER [UninstallRun], so
; restore still finds what it needs.
Root: HKLM; Subkey: "SOFTWARE\CatCam"; Flags: uninsdeletekey

[Tasks]
Name: "usb"; Description: "USB cable mode (downloads Google platform-tools; Wi-Fi mode needs no cable)"; Flags: unchecked

[Icons]
Name: "{autoprograms}\CatCam"; Filename: "{app}\CatCamTray.exe"; IconFilename: "{app}\catcam.ico"
; Support path: writes a diagnostics zip to the Desktop for GitHub issues.
Name: "{autoprograms}\CatCam Diagnostics"; Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\catcam-diag.ps1"""; IconFilename: "{app}\catcam.ico"

[Run]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\CatCamSource.dll"""; StatusMsg: "Registering the virtual camera..."
; Task creation lives in PowerShell: Register-ScheduledTask quotes the
; spaces-in-Program-Files path correctly, where raw schtasks /tr quoting
; silently broke (found by the VM test). The script also starts the task.
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\task-setup.ps1"" -Boot ""{app}\catcam-boot.bat"""; StatusMsg: "Creating the startup task..."
; Microphone support is part of EVERY install (idempotent: skips if
; VB-Cable is present). Silent-first via VB's VM-verified -i -h switches;
; the interactive GUI fallback is only allowed when a user is present.
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\mic-setup.ps1"" -AllowGui"; StatusMsg: "Setting up microphone support..."; Check: not WizardSilent
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\mic-setup.ps1"""; StatusMsg: "Setting up microphone support..."; Check: WizardSilent
; Make the cable's endpoints read as CatCam's: "CatCam Microphone" (what you
; pick in Teams), "CatCam Mic Feed" (what the host writes into), the unused
; 16ch endpoints disabled, the feed's endpoint ID pinned for the host.
; Endpoint names/states are Windows-side per-endpoint settings (the Sound
; panel's own Rename/Disable); VB-Cable's driver is untouched. Idempotent;
; a no-op with a message if VB-Cable is not present.
Filename: "{app}\CatCamAudio.exe"; Parameters: "setup"; StatusMsg: "Naming the CatCam microphone..."; Flags: runhidden
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\usb-setup.ps1"" -InstallDir ""{app}"""; StatusMsg: "Fetching platform-tools for USB mode..."; Tasks: usb

[UninstallRun]
; One cmd: kill both, then a ping-delay so the exes actually release their
; file locks before Inno deletes (taskkill /F returns before process death;
; without the delay the uninstall left locked files behind, VM-tested).
; ping not timeout.exe: timeout dies without console stdin (HANDOFF lesson).
Filename: "{cmd}"; Parameters: "/c taskkill /F /IM CatCamTray.exe & taskkill /F /IM CatCamHost.exe & taskkill /F /IM adb.exe & ping -n 3 127.0.0.1 > nul"; Flags: runhidden; RunOnceId: "KillProcs"
Filename: "{sys}\schtasks.exe"; Parameters: "/delete /tn CatCam /f"; Flags: runhidden; RunOnceId: "DelTask"
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\CatCamSource.dll"""; Flags: runhidden; RunOnceId: "UnregDll"
; Give VB-Cable its own names back and re-enable the 16ch endpoints.
Filename: "{app}\CatCamAudio.exe"; Parameters: "restore"; Flags: runhidden; RunOnceId: "AudioRestore"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
