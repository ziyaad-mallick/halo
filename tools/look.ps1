# Launch halo (optionally with env vars), screenshot the dock at the bottom
# edge and while hovering, then close it. Refuses to run on the lock screen.
# Usage: tools/look.ps1 -Tag name [-Env @{HALO_WARP='1'}] [-Exe build/halo.exe]
param([string]$Tag = 'look', [hashtable]$Env = @{}, [string]$Exe = '')
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
if (-not $Exe) { $Exe = Join-Path $out 'halo.exe' }
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Lk {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
}
'@
[Lk]::SetProcessDPIAware() | Out-Null
$sb = New-Object System.Text.StringBuilder 200
[Lk]::GetWindowText([Lk]::GetForegroundWindow(), $sb, 200) | Out-Null
if ($sb.ToString() -match 'Lock Screen') { "SKIPPED: the PC is locked ($sb)"; return }

function Shot($name) {
    $b = New-Object System.Drawing.Bitmap 820, 200; $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen(560, 1000, 0, 0, $b.Size); $p = Join-Path $out "$Tag-$name.png"; $b.Save($p); $g.Dispose(); $b.Dispose(); $p
}
foreach ($k in $Env.Keys) { Set-Item "Env:$k" $Env[$k] }
[Lk]::SetCursorPos(960, 500) | Out-Null
$p = Start-Process $Exe -PassThru -RedirectStandardError (Join-Path $out "$Tag.log")
Start-Sleep 2
[Lk]::SetCursorPos(700, 1199) | Out-Null; Start-Sleep -Milliseconds 800; Shot 'edge'
for ($x = 700; $x -le 1000; $x += 4) { [Lk]::SetCursorPos($x, 1150) | Out-Null; Start-Sleep -Milliseconds 8 }
Start-Sleep -Milliseconds 400; Shot 'hover'
$g = Get-Process -Id $p.Id
"private {0:N1} MB, working set {1:N1} MB" -f ($g.PrivateMemorySize64 / 1MB), ($g.WorkingSet64 / 1MB)
[Lk]::SetCursorPos(960, 500) | Out-Null; Start-Sleep 1
Stop-Process -Id $p.Id
foreach ($k in $Env.Keys) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
