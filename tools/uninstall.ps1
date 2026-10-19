# Remove the installed copy that tools/install.ps1 made: stops guardian, halo and halo-settings, puts
# Windows' taskbar back, deletes the logon entry (HKCU Run "Halo"), the Halo Settings Start-menu shortcut
# and %LOCALAPPDATA%\Halo\app. Settings, logs and the taskbar state file in %LOCALAPPDATA%\Halo stay unless
# -RemoveData is given.
#   tools/uninstall.ps1 [-RemoveData]
param([switch]$RemoveData)
$ErrorActionPreference = 'Continue'
$root = Join-Path $env:LOCALAPPDATA 'Halo'
$app = Join-Path $root 'app'
$guardian = Join-Path $app 'guardian.exe'

# Logon entry first, so nothing restarts the guardian while we work (guardian.exe --autostart off does the
# same through its own code; the direct delete covers a missing exe).
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Halo' -ErrorAction SilentlyContinue

# Only the installed copies: test builds from build/ may be running too.
$live = { Get-Process halo, guardian, halo-settings -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$app\*" } }

# Ask the running guardian to quit: it closes halo and restores the taskbar itself.
if ((Test-Path $guardian) -and (Get-Process guardian -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$app\*" })) {
    & $guardian --quit | Out-Null
    $deadline = (Get-Date).AddSeconds(10)
    while ((& $live | Where-Object { $_.Name -ne 'halo-settings' }) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 200 }
}
& $live | Stop-Process -Force -ErrorAction SilentlyContinue

# Whatever happened above, the taskbar comes back: --restore works with no guardian running (it also
# closes a stray halo.exe).
if (Test-Path $guardian) {
    & $guardian --restore | Out-Null
} else {
    Write-Warning 'guardian.exe is missing; if the taskbar is still hidden, sign out and back in or restart explorer.exe.'
}

# Halo Settings' Start-menu shortcut (written by halo-settings.exe --register).
$lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'Halo Settings.lnk'
if (Test-Path $lnk) { Remove-Item $lnk -Force -ErrorAction SilentlyContinue }

# The image stays locked for a moment after the process is gone.
if (Test-Path $app) {
    for ($i = 0; ; $i++) {
        try { Remove-Item $app -Recurse -Force -ErrorAction Stop; break }
        catch { if ($i -ge 20) { Write-Warning "could not remove $app : $_"; break }; Start-Sleep -Milliseconds 250 }
    }
}
if ($RemoveData -and (Test-Path $root)) { Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue }
Write-Host 'Halo removed.' $(if (!$RemoveData) { "Settings and logs kept in $root (-RemoveData deletes them)." })
