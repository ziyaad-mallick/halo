# Smoke test: launch halo, screenshot at rest and while magnified, report
# memory/CPU, close it. Moves the mouse. Usage: tools/smoke.ps1 [-Tag name]
param([string]$Tag = 'smoke', [int]$HoverX = 960, [int]$HoverY = 1150)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Threading;
public static class Sm {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
  // The dock window of a process (FindWindow by class alone misses it).
  public static IntPtr DockWindow(uint target) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid);
      var sb = new System.Text.StringBuilder(64); GetClassName(h, sb, 64);
      if (pid == target && sb.ToString() == "HaloDockWindow") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
'@
[Sm]::SetProcessDPIAware() | Out-Null
function Shot($name) {
    $bmp = New-Object System.Drawing.Bitmap 1920, 200
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen(0, 1000, 0, 0, $bmp.Size)
    $p = Join-Path $out "$Tag-$name.png"; $bmp.Save($p); $g.Dispose(); $bmp.Dispose(); $p
}

[Sm]::SetCursorPos(960, 400) | Out-Null
$p = Start-Process (Join-Path $out 'halo.exe') -PassThru -RedirectStandardError (Join-Path $out "$Tag.log")
Start-Sleep -Seconds 3
Shot 'rest'
$proc = Get-Process -Id $p.Id
"memory: working set {0:N1} MB, private {1:N1} MB, threads {2}" -f ($proc.WorkingSet64 / 1MB), ($proc.PrivateMemorySize64 / 1MB), $proc.Threads.Count
$cpu0 = $proc.TotalProcessorTime.TotalMilliseconds
Start-Sleep -Seconds 10
$proc.Refresh()
"idle CPU over 10 s: {0:N0} ms" -f ($proc.TotalProcessorTime.TotalMilliseconds - $cpu0)

$cpu0 = $proc.TotalProcessorTime.TotalMilliseconds
for ($x = $HoverX - 250; $x -le $HoverX + 250; $x += 4) { [Sm]::SetCursorPos($x, $HoverY) | Out-Null; Start-Sleep -Milliseconds 8 }
for ($x = $HoverX + 250; $x -ge $HoverX; $x -= 4) { [Sm]::SetCursorPos($x, $HoverY) | Out-Null; Start-Sleep -Milliseconds 8 }
Start-Sleep -Milliseconds 400
Shot 'hover'
$proc.Refresh()
"CPU during ~2 s hover sweep: {0:N0} ms" -f ($proc.TotalProcessorTime.TotalMilliseconds - $cpu0)
[Sm]::SetCursorPos(960, 400) | Out-Null
Start-Sleep -Milliseconds 600
Shot 'after'

[Sm]::PostMessage([Sm]::DockWindow([uint32]$p.Id), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 1
if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $p.Id; "halo did not close on WM_CLOSE; killed" } else { "halo closed" }
"--- log:"; Get-Content (Join-Path $out "$Tag.log")
