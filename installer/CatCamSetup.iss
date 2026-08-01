; CatCam one-click installer (Inno Setup 6).
; Replaces the elevated-PowerShell ceremony of install.ps1 with a wizard:
; copies the runtime to Program Files (which LOCAL SERVICE can read, so the
; old icacls grant is unnecessary), registers the COM media source, creates
; the elevated logon task, optionally sets up the microphone path (VB-Audio
; Virtual Cable, fetched from vb-audio.com, never bundled: their license
; forbids redistribution) and optionally fetches Google platform-tools for
; USB cable mode. Wi-Fi mode needs neither optional piece.
; Build: iscc CatCamSetup.iss   (output: Output\CatCamSetup.exe)

[Setup]
AppId={{7C1FA2E5-4B3D-4A80-9E6F-CATCAM000001}
AppName=CatCam
AppVersion=1.1.0
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
Source: "..\windows\catcam-boot.bat";  DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\camctl.bat";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\adbfwd.bat";       DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\catcam-env.bat";   DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\catcam64.png";     DestDir: "{app}"; Flags: ignoreversion
Source: "..\windows\probe.py";         DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE";                  DestDir: "{app}"; Flags: ignoreversion
Source: "..\NOTICE";                   DestDir: "{app}"; Flags: ignoreversion
Source: "mic-setup.ps1";               DestDir: "{app}"; Flags: ignoreversion
Source: "usb-setup.ps1";               DestDir: "{app}"; Flags: ignoreversion
Source: "task-setup.ps1";              DestDir: "{app}"; Flags: ignoreversion
Source: "catcam.ico";                  DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: "mic"; Description: "Microphone support (downloads VB-Audio Virtual Cable from vb-audio.com, their installer will open)"; Flags: unchecked
Name: "usb"; Description: "USB cable mode (downloads Google platform-tools; Wi-Fi mode needs no cable)"; Flags: unchecked

[Icons]
Name: "{autoprograms}\CatCam"; Filename: "{app}\CatCamTray.exe"; IconFilename: "{app}\catcam.ico"

[Run]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\CatCamSource.dll"""; StatusMsg: "Registering the virtual camera..."
; Task creation lives in PowerShell: Register-ScheduledTask quotes the
; spaces-in-Program-Files path correctly, where raw schtasks /tr quoting
; silently broke (found by the VM test). The script also starts the task.
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\task-setup.ps1"" -Boot ""{app}\catcam-boot.bat"""; StatusMsg: "Creating the startup task..."
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\mic-setup.ps1"""; StatusMsg: "Setting up microphone support..."; Tasks: mic
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\usb-setup.ps1"" -InstallDir ""{app}"""; StatusMsg: "Fetching platform-tools for USB mode..."; Tasks: usb

[UninstallRun]
; One cmd: kill both, then a ping-delay so the exes actually release their
; file locks before Inno deletes (taskkill /F returns before process death;
; without the delay the uninstall left locked files behind, VM-tested).
; ping not timeout.exe: timeout dies without console stdin (HANDOFF lesson).
Filename: "{cmd}"; Parameters: "/c taskkill /F /IM CatCamTray.exe & taskkill /F /IM CatCamHost.exe & ping -n 3 127.0.0.1 > nul"; Flags: runhidden; RunOnceId: "KillProcs"
Filename: "{sys}\schtasks.exe"; Parameters: "/delete /tn CatCam /f"; Flags: runhidden; RunOnceId: "DelTask"
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\CatCamSource.dll"""; Flags: runhidden; RunOnceId: "UnregDll"

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
