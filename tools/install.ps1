# Ship a build to the installed copy that runs at logon (%LOCALAPPDATA%\Halo\app,
# HKCU Run "Halo" -> guardian.exe there). Quits the running guardian (taskbar comes
# back for a moment), copies the exes, starts the guardian again.
#   tools/install.ps1 [-Halo build/halo.exe] [-Guardian build/guardian.exe]
param([string]$Halo = '', [string]$Guardian = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$Halo) { $Halo = Join-Path $repo 'build\halo.exe' }
if (!$Guardian) { $Guardian = Join-Path $repo 'build\guardian.exe' }
$app = Join-Path $env:LOCALAPPDATA 'Halo\app'
New-Item -ItemType Directory -Force $app | Out-Null

$running = Join-Path $app 'guardian.exe'
if (Get-Process guardian -ErrorAction SilentlyContinue) {
    if (Test-Path $running) { & $running --quit | Out-Null } else { & $Guardian --quit | Out-Null }
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Process halo, guardian -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
}
Copy-Item $Halo (Join-Path $app 'halo.exe') -Force
Copy-Item $Guardian (Join-Path $app 'guardian.exe') -Force
Start-Process $running
Start-Sleep 3
Get-Process halo, guardian -ErrorAction SilentlyContinue | Select-Object Name, Id, Path
Get-Content (Join-Path $env:LOCALAPPDATA 'Halo\guardian.log') -Tail 3
