<#
CatCam site deploy. Run from any checkout of this repo:
    pwsh -File scripts\deploy-site.ps1

The live site (https://catcam.app) is a handful of static files served by
caddy from /opt/apps/ai-trading-desk/web/portal/catcam on the hosting box,
a subfolder of an existing directory mount: files land, caddy serves them,
no container is touched. A portal redeploy that rsyncs with --delete wipes
that subfolder; rerunning this script restores it from scratch.

What it does, in order:
  1. Refuses to start while any SSM command is still running on the box
     (a second command sent mid-run has broken this host before).
  2. Sends one AWS-RunShellScript: fetch every file in the manifest from
     this repo's master on raw.githubusercontent.com into <name>.new,
     verify the downloads (html closes, images carry their magic bytes),
     then move them into place. Nothing live is replaced until every
     download passed.
  3. Polls the command to completion and prints the box-side output.
  4. Downloads every file back from https://catcam.app and compares git
     blob ids against local master, so a forgotten push shows up here.

It ships what MASTER has, not your working tree: commit and push first.
Needs the local aws CLI with credentials for the hosting account.
#>
param(
    [string]$InstanceId = 'i-0616d34cd058d4125',
    [string]$Region     = 'us-east-1'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$dest = '/opt/apps/ai-trading-desk/web/portal/catcam'
$raw  = 'https://raw.githubusercontent.com/igorfyago/CatCam/master'

function Invoke-Aws {
    param([Parameter(Mandatory)][string[]]$CliArgs)
    $out = & aws @CliArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "aws $($CliArgs[0]) $($CliArgs[1]) failed:`n$($out -join [Environment]::NewLine)"
    }
    $out | Where-Object { $_ -isnot [System.Management.Automation.ErrorRecord] }
}

# Manifest: everything tracked under site/, plus the logo, which lives in
# assets/ and is referenced by the pages as a bare filename.
$siteFiles = @(git -C $root ls-files site)
if ($LASTEXITCODE -ne 0 -or $siteFiles.Count -eq 0) {
    throw 'git ls-files site came back empty, run this from a CatCam checkout'
}
$nested = @($siteFiles | Where-Object { $_ -match '^site/.+/' })
if ($nested.Count) {
    throw "site/ grew subdirectories, this script deploys a flat directory: $($nested -join ', ')"
}
$manifest = @($siteFiles | ForEach-Object { [pscustomobject]@{ Repo = $_; Name = Split-Path $_ -Leaf } })
$manifest += [pscustomobject]@{ Repo = 'assets/logo512.png'; Name = 'logo512.png' }
foreach ($f in $manifest) {
    if ($f.Name -notmatch '^[A-Za-z0-9._-]+$') { throw "filename not shell-safe, not deploying it: $($f.Repo)" }
}
$dupes = @($manifest | Group-Object Name | Where-Object Count -gt 1)
if ($dupes.Count) {
    throw "two sources map to one live filename ($($dupes.Name -join ', ')): if the logo moved into site/, drop the assets/ line above"
}

$dirty = @(git -C $root status --porcelain -- site assets/logo512.png)
if ($dirty.Count) {
    Write-Host 'NOTE: local changes below are NOT what ships, the box pulls master:' -ForegroundColor Yellow
    $dirty | ForEach-Object { Write-Host "  $_" -ForegroundColor Yellow }
}

Write-Host "== SSM busy check on $InstanceId"
$busy = ((Invoke-Aws @('ssm','list-command-invocations','--region',$Region,'--instance-id',$InstanceId,
        '--query',"CommandInvocations[?Status=='Pending' || Status=='InProgress' || Status=='Delayed'].CommandId",
        '--output','text')) -join ' ').Trim()
if ($busy -and $busy -ne 'None') {
    Write-Host "Another SSM command is still running on the box: $busy" -ForegroundColor Red
    Write-Host 'Wait for it to finish (aws ssm list-command-invocations) and rerun.'
    exit 1
}

# One remote script, three phases: fetch all, verify all, move all.
# set -e aborts before anything live is replaced if any step fails, and
# the trap clears the .new staging files on any exit.
$fetch   = $manifest | ForEach-Object { "curl -fsSL --retry 3 '$raw/$($_.Repo)' -o '$($_.Name).new'" }
$verify  = $manifest | ForEach-Object {
    $n = $_.Name   # switch rebinds $_ inside its case blocks
    switch -Wildcard ($n) {
        '*.html' { "grep -q '</html>' '${n}.new'" }
        '*.png'  { "head -c 8 '${n}.new' | grep -q PNG" }
        '*.webp' { "head -c 12 '${n}.new' | grep -q WEBP" }
        default  { "test -s '${n}.new'" }
    }
}
$install  = $manifest | ForEach-Object { "chmod 644 '$($_.Name).new' && mv '$($_.Name).new' '$($_.Name)'" }
$commands = @('set -eu', "mkdir -p $dest", "cd $dest", "trap 'rm -f *.new' EXIT") +
            $fetch + $verify + $install + @('ls -la')

$payloadFile = Join-Path ([IO.Path]::GetTempPath()) 'catcam-site-deploy.json'
@{
    DocumentName = 'AWS-RunShellScript'
    InstanceIds  = @($InstanceId)
    Comment      = 'CatCam site deploy (scripts/deploy-site.ps1)'
    Parameters   = @{ commands = @($commands); executionTimeout = @('120') }
} | ConvertTo-Json -Depth 5 | Set-Content -Path $payloadFile -Encoding ascii

Write-Host "== Deploying $($manifest.Count) files to $dest"
$cmdId = ((Invoke-Aws @('ssm','send-command','--region',$Region,'--cli-input-json',"file://$payloadFile",
          '--query','Command.CommandId','--output','text')) -join '').Trim()
Write-Host "== Command $cmdId sent, polling"

$inv = $null
$deadline = (Get-Date).AddMinutes(3)
do {
    Start-Sleep -Seconds 3
    try {
        $inv = ((Invoke-Aws @('ssm','get-command-invocation','--region',$Region,'--output','json',
                '--command-id',$cmdId,'--instance-id',$InstanceId)) -join "`n") | ConvertFrom-Json
    } catch { $inv = $null }   # the invocation registers a moment after send-command
} while ((-not $inv -or $inv.Status -in 'Pending','InProgress','Delayed') -and (Get-Date) -lt $deadline)

if (-not $inv -or $inv.Status -in 'Pending','InProgress','Delayed') {
    throw "command $cmdId is still running on the box. Do NOT resend: this poll giving up does not end the box-side command. Watch it with: aws ssm list-command-invocations --region $Region --instance-id $InstanceId"
}
if ($inv.StandardOutputContent) { Write-Host $inv.StandardOutputContent }
if ($inv.Status -ne 'Success') {
    if ($inv.StandardErrorContent) { Write-Host $inv.StandardErrorContent -ForegroundColor Red }
    throw "deploy ended $($inv.Status): live files were not replaced, staging was cleaned up"
}

Write-Host '== Live check against https://catcam.app'
$bad = @()
foreach ($f in $manifest) {
    $tmp = Join-Path ([IO.Path]::GetTempPath()) "catcam-live-$($f.Name)"
    Invoke-WebRequest -Uri "https://catcam.app/$($f.Name)" -OutFile $tmp -UseBasicParsing
    # Compare git blob ids, byte for byte: this checkout's files can carry
    # CRLF line endings that the committed blobs (and the live site) do not,
    # so hashing the working tree files would false-alarm on every html.
    $want = "$(git -C $root rev-parse "master:$($f.Repo)" 2>$null)".Trim()
    $got  = "$(git -C $root hash-object --no-filters $tmp)".Trim()
    Remove-Item $tmp -ErrorAction SilentlyContinue
    if ($want -and $got -eq $want) { Write-Host "  OK        $($f.Name)" }
    else { Write-Host "  MISMATCH  $($f.Name)" -ForegroundColor Red; $bad += $f.Name }
}
if ($bad.Count) {
    Write-Host 'Live differs from local master: unpushed commits, a file master does not have,' -ForegroundColor Yellow
    Write-Host 'or raw.githubusercontent.com still caching the pre-push blob. Push, give it a minute, rerun.' -ForegroundColor Yellow
    exit 1
}
Write-Host '== Live and verified: https://catcam.app serves this tree'
