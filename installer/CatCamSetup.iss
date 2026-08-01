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
Source: "catcam.ico";                  DestDir: "{app}"; Flags: ignoreversion

[Tasks]
Name: "mic"; Description: "Microphone support (downloads VB-Audio Virtual Cable from vb-audio.com, their installer will open)"; Flags: unchecked
Name: "usb"; Description: "USB cable mode (downloads Google platform-tools; Wi-Fi mode needs no cable)"; Flags: unchecked

[Icons]
Name: "{autoprograms}\CatCam"; Filename: "{app}\CatCamTray.exe"; IconFilename: "{app}\catcam.ico"

[Run]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\CatCamSource.dll"""; StatusMsg: "Registering the virtual camera..."
Filename: "{sys}\schtasks.exe"; Parameters: "/create /tn CatCam /sc onlogon /rl highest /tr """"{app}\catcam-boot.bat"""" /f"; StatusMsg: "Creating the startup task..."
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\mic-setup.ps1"""; StatusMsg: "Setting up microphone support..."; Tasks: mic
Filename: "powershell.exe"; Parameters: "-ExecutionPolicy Bypass -File ""{app}\usb-setup.ps1"" -InstallDir ""{app}"""; StatusMsg: "Fetching platform-tools for USB mode..."; Tasks: usb
Filename: "{sys}\schtasks.exe"; Parameters: "/run /tn CatCam"; StatusMsg: "Starting CatCam..."; Flags: nowait

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM CatCamTray.exe"; Flags: runhidden; RunOnceId: "KillTray"
Filename: "{sys}\taskkill.exe"; Parameters: "/F /IM CatCamHost.exe"; Flags: runhidden; RunOnceId: "KillHost"
Filename: "{sys}\schtasks.exe"; Parameters: "/delete /tn CatCam /f"; Flags: runhidden; RunOnceId: "DelTask"
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\CatCamSource.dll"""; Flags: runhidden; RunOnceId: "UnregDll"

[UninstallDelete]
Type: files; Name: "{app}\catcam.env.bat"
Type: files; Name: "{app}\tray.log"
Type: files; Name: "{app}\host.log"
Type: filesandordirs; Name: "{app}\platform-tools"
