# Create (or replace) the CatCam logon task and start it now.
#
# Explicit task XML, for three hard-won reasons (VM-tested):
# - schtasks /tr quoting of a spaces-in-Program-Files path silently breaks.
# - New-ScheduledTaskTrigger -AtLogOn bakes the CALLING user into the
#   trigger; under SYSTEM (silent/automated installs) that UserId cannot be
#   mapped and registration fails with 0x80070534.
# - The principal must be the INTERACTIVE user whenever one exists: the tray
#   lives in the user's session; a SYSTEM task would run it invisibly in
#   session 0. Under a real (elevated) install the current user IS the right
#   principal; only headless SYSTEM contexts fall back to ServiceAccount.
param([Parameter(Mandatory)][string]$Boot)
$ErrorActionPreference = 'Stop'
$sid = [System.Security.Principal.WindowsIdentity]::GetCurrent().User.Value
# SYSTEM principals must OMIT LogonType entirely (the XML schema rejects
# "ServiceAccount" there); interactive users get InteractiveToken so the
# tray runs in their session.
$logonXml = if ($sid -eq 'S-1-5-18') { '' } else { '<LogonType>InteractiveToken</LogonType>' }
$xml = @"
<?xml version="1.0" encoding="UTF-16"?>
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <Triggers>
    <LogonTrigger><Enabled>true</Enabled></LogonTrigger>
  </Triggers>
  <Principals>
    <Principal id="Author">
      <UserId>$sid</UserId>
      $logonXml
      <RunLevel>HighestAvailable</RunLevel>
    </Principal>
  </Principals>
  <Settings>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>
    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <StartWhenAvailable>true</StartWhenAvailable>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
  </Settings>
  <Actions Context="Author">
    <Exec>
      <Command>"$Boot"</Command>
    </Exec>
  </Actions>
</Task>
"@
try {
    Register-ScheduledTask -TaskName 'CatCam' -Xml $xml -Force | Out-Null
    Start-ScheduledTask -TaskName 'CatCam'
    Write-Host "CatCam startup task created and started."
} catch {
    Write-Host "Task setup failed: $_"
    exit 1
}
