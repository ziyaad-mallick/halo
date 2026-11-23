# Run before installing a build: refuses if the build would drop installed work.
# Reads the last row of INSTALLED.md (date | branch | commit | md5 | rollback dir) and checks
#   git merge-base --is-ancestor <installed commit> <Head>
# Exit 0: safe. Exit 1: the installed commit is not in Head (lists what would be dropped). Exit 2: no usable ledger.
#   tools/preflight.ps1 [-Head HEAD] [-Ledger path\to\INSTALLED.md]
# The ledger lives in the primary checkout, so every worktree reads and writes the same file.
param([string]$Head = 'HEAD', [string]$Ledger = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$Ledger) {
    $common = (& git -C $repo rev-parse --path-format=absolute --git-common-dir).Trim()
    $Ledger = Join-Path (Split-Path $common) 'INSTALLED.md'
}
if (!(Test-Path $Ledger)) { Write-Host "preflight: no ledger at $Ledger"; exit 2 }
$row = Get-Content $Ledger | Where-Object { $_ -match '^\d{4}-\d{2}-\d{2}' } | Select-Object -Last 1
if (!$row) { Write-Host "preflight: no install rows in $Ledger"; exit 2 }
$f = $row -split '\s*\|\s*'
$installed = $f[2]
& git -C $repo cat-file -e "$installed^{commit}" 2>$null
if ($LASTEXITCODE -ne 0) { Write-Host "preflight: installed commit '$installed' is not in this repo (last row: $row)"; exit 2 }
& git -C $repo merge-base --is-ancestor $installed $Head
if ($LASTEXITCODE -eq 0) { Write-Host "preflight OK: installed $installed ($($f[1])) is an ancestor of $Head"; exit 0 }
Write-Host "preflight FAILED: installed $installed ($($f[1])) is not an ancestor of $Head."
Write-Host "Installing $Head would drop these installed commits:"
& git -C $repo log --oneline "$Head..$installed"
exit 1
