# Ship a build to the installed copy that runs at logon (%LOCALAPPDATA%\Halo\app,
# HKCU Run "Halo" -> guardian.exe there). Quits the running guardian (taskbar comes
# back for a moment), copies the exes, starts the guardian again.
#   tools/install.ps1 [-Halo build/halo.exe] [-Guardian build/guardian.exe] [-Settings build/halo-settings.exe]
param([string]$Halo = '', [string]$Guardian = '', [string]$Settings = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$Halo) { $Halo = Join-Path $repo 'build\halo.exe' }
if (!$Guardian) { $Guardian = Join-Path $repo 'build\guardian.exe' }
if (!$Settings) { $Settings = Join-Path $repo 'build\halo-settings.exe' }
$app = Join-Path $env:LOCALAPPDATA 'Halo\app'
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
foreach ($pair in @(@($Halo, 'halo.exe'), @($Guardian, 'guardian.exe'), @($Settings, 'halo-settings.exe'))) {
    if ($pair[1] -eq 'halo-settings.exe' -and !(Test-Path $pair[0])) { continue }
    for ($i = 0; ; $i++) {
        try { Copy-Item $pair[0] (Join-Path $app $pair[1]) -Force; break }
        catch { if ($i -ge 20) { throw }; Start-Sleep -Milliseconds 250 }
    }
}
# Halo Settings: a Start-menu shortcut (skipped if the exe wasn't built).
if (Test-Path (Join-Path $app 'halo-settings.exe')) {
    $lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'Halo Settings.lnk'
    $sc = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
    $sc.TargetPath = Join-Path $app 'halo-settings.exe'
    $sc.WorkingDirectory = $app
    $sc.Description = 'Customise Halo'
    $sc.Save()
}
Start-Process $running
Start-Sleep 3
Get-Process halo, guardian -ErrorAction SilentlyContinue | Select-Object Name, Id, Path
Get-Content (Join-Path $env:LOCALAPPDATA 'Halo\guardian.log') -Tail 3
