# Ship a build to the installed copy that runs at logon (%LOCALAPPDATA%\Halo\app,
# HKCU Run "Halo" -> guardian.exe there). Quits the running guardian (taskbar comes
# back for a moment), copies the exes, starts the guardian again.
#   tools/install.ps1 [-Halo build/halo.exe] [-Guardian build/guardian.exe] [-Settings build/halo-settings.exe]
param([string]$Halo = '', [string]$Guardian = '', [string]$Settings = '', [switch]$DryRun)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
# Inside the release zip the exes sit next to this script; in a clone they are in build\.
$bin = if (Test-Path (Join-Path $PSScriptRoot 'halo.exe')) { $PSScriptRoot } else { Join-Path $repo 'build' }
if (!$Halo) { $Halo = Join-Path $bin 'halo.exe' }
if (!$Guardian) { $Guardian = Join-Path $bin 'guardian.exe' }
if (!$Settings) { $Settings = Join-Path $bin 'halo-settings.exe' }
$app = Join-Path $env:LOCALAPPDATA 'Halo\app'
# Rollback first, ledger last: the whole installed app dir is copied to app-backup-<stamp> before anything is replaced,
# and one row (date | branch | commit | md5 | rollback dir) is appended to INSTALLED.md in the primary checkout after.
# Run tools/preflight.ps1 before installing a build. -DryRun prints the rollback dir and ledger row and touches nothing.
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$rollback = "app-backup-$stamp"
$hasGit = Test-Path (Join-Path $repo '.git')
$ledger = ''
if ($hasGit) {
    $common = (& git -C $repo rev-parse --path-format=absolute --git-common-dir).Trim()
    $ledger = Join-Path (Split-Path $common) 'INSTALLED.md'
}
$ledgerRow = {
    $commit = (& git -C $repo rev-parse --short=7 HEAD).Trim()
    $branch = (& git -C $repo branch --show-current).Trim()
    $md5 = (Get-FileHash (Join-Path $app 'halo.exe') -Algorithm MD5).Hash.Substring(0, 8)
    "$(Get-Date -Format 'yyyy-MM-dd HH:mm') | $branch | $commit | $md5 | $rollback"
}
if ($DryRun) {
    Write-Host "DryRun: would copy $app -> $(Join-Path (Split-Path $app) $rollback)"
    if ($hasGit) {
        $md5 = (Get-FileHash $Halo -Algorithm MD5).Hash.Substring(0, 8)
        Write-Host "DryRun: would append to $ledger : $(Get-Date -Format 'yyyy-MM-dd HH:mm') | $((& git -C $repo branch --show-current).Trim()) | $((& git -C $repo rev-parse --short=7 HEAD).Trim()) | $md5 | $rollback"
    } else { Write-Host 'DryRun: no .git next to this script (release zip), no ledger row' }
    exit 0
}
New-Item -ItemType Directory -Force $app | Out-Null

$running = Join-Path $app 'guardian.exe'
if (Get-Process guardian -ErrorAction SilentlyContinue) {
    if (Test-Path $running) { & $running --quit | Out-Null } else { & $Guardian --quit | Out-Null }
    $deadline = (Get-Date).AddSeconds(10)
    # Only the installed copies: test builds from build/ may be running too.
    $live = { Get-Process halo, guardian -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$app\*" } }
    while ((& $live) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
}
# The guardian may have just TerminateProcess'd a halo that ignored WM_CLOSE; the
# image stays locked for a moment after the process is gone.
$rollbackPath = Join-Path (Split-Path $app) $rollback
if (Test-Path $rollbackPath) { throw "rollback dir already exists: $rollbackPath" }
Copy-Item $app $rollbackPath -Recurse
# One step back: the exe being replaced stays as halo.previous.exe (skipped when that file is what's being installed).
$installed = Join-Path $app 'halo.exe'
if ((Test-Path $installed) -and ((Resolve-Path $Halo).Path -ne (Join-Path $app 'halo.previous.exe'))) {
    Copy-Item $installed (Join-Path $app 'halo.previous.exe') -Force -ErrorAction SilentlyContinue
}
foreach ($pair in @(@($Halo, 'halo.exe'), @($Guardian, 'guardian.exe'), @($Settings, 'halo-settings.exe'))) {
    if ($pair[1] -eq 'halo-settings.exe' -and !(Test-Path $pair[0])) { continue }
    for ($i = 0; ; $i++) {
        try { Copy-Item $pair[0] (Join-Path $app $pair[1]) -Force; break }
        catch { if ($i -ge 20) { throw }; Start-Sleep -Milliseconds 250 }
    }
}
if ($hasGit) { Add-Content -Path $ledger -Value (& $ledgerRow) -Encoding utf8 }
# Halo Preferences: its Start-menu shortcut, which carries its app ID (skipped if the exe wasn't built).
if (Test-Path (Join-Path $app 'halo-settings.exe')) {
    Start-Process (Join-Path $app 'halo-settings.exe') -ArgumentList '--register' -Wait
}
Start-Process $running
Start-Sleep 3
Get-Process halo, guardian -ErrorAction SilentlyContinue | Select-Object Name, Id, Path
Get-Content (Join-Path $env:LOCALAPPDATA 'Halo\guardian.log') -Tail 3
