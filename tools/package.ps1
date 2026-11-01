# Packs the release zip: the three programs plus the installer, symbols stripped. Builds nothing, so run
# tools/build.sh all first.   tools/package.ps1 [-Out dist\halo-win-x64.zip]
param([string]$Out = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$Out) { $Out = Join-Path $repo 'dist\halo-win-x64.zip' }
$stage = Join-Path $env:TEMP "halo-package-$PID"
New-Item -ItemType Directory -Force $stage, (Split-Path $Out) | Out-Null
foreach ($f in 'halo.exe', 'guardian.exe', 'halo-settings.exe') {
    $src = Join-Path $repo "build\$f"
    if (!(Test-Path $src)) { throw "$src is missing: run tools/build.sh all" }
    Copy-Item $src $stage
}
$strip = Get-ChildItem (Join-Path $repo 'toolchain\llvm-mingw\bin') -Filter 'llvm-strip.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($strip) { Get-ChildItem $stage -Filter *.exe | ForEach-Object { & $strip.FullName --strip-all $_.FullName } }
Copy-Item (Join-Path $PSScriptRoot 'install.ps1'), (Join-Path $PSScriptRoot 'uninstall.ps1'), (Join-Path $repo 'LICENSE') $stage
if (Test-Path $Out) { Remove-Item $Out }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $Out -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force
Get-Item $Out | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 2) } }
(Get-FileHash $Out -Algorithm SHA256).Hash
