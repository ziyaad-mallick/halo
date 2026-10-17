# The M0 checks guarded-run.ps1 does not cover, run under the guardian:
#  1. every taskbar (primary + secondary monitors) is hidden, then restored
#  2. the reveal spring overshoots: a burst of frames, dock top edge per frame
#  3. -ExplorerRestart: explorer is restarted mid-run and the new taskbar is
#     hidden again while halo keeps running
# Moves the mouse. Refuses to run on the lock screen.
param([switch]$ExplorerRestart)
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices; using System.Collections.Generic;
public static class Mc {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr p, IntPtr a, string c, string n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  // "primary=True secondary=[True,True]" for every taskbar window.
  public static string Taskbars() {
    var parts = new List<string>();
    IntPtr h = IntPtr.Zero;
    while ((h = FindWindowEx(IntPtr.Zero, h, "Shell_SecondaryTrayWnd", null)) != IntPtr.Zero) parts.Add(IsWindowVisible(h).ToString());
    return "primary=" + IsWindowVisible(FindWindowEx(IntPtr.Zero, IntPtr.Zero, "Shell_TrayWnd", null)) + " secondary=[" + string.Join(",", parts) + "]";
  }
  public static IntPtr DockWindow(uint target) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid); var sb = new StringBuilder(64); GetClassName(h, sb, 64);
      if (pid == target && sb.ToString() == "HaloDockWindow") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
'@
[Mc]::SetProcessDPIAware() | Out-Null
$sb = New-Object System.Text.StringBuilder 200
[Mc]::GetWindowText([Mc]::GetForegroundWindow(), $sb, 200) | Out-Null
if ($sb.ToString() -match 'Lock Screen') { "SKIPPED: the PC is locked"; return }

# A strip under the dock's centre; the dock top is the first row (from the top)
# that differs from the hidden-state frame.
$X0 = 760; $Y0 = 1040; $W = 400; $H = 160
function Grab { $b = New-Object System.Drawing.Bitmap $W, $H; $g = [System.Drawing.Graphics]::FromImage($b); $g.CopyFromScreen($X0, $Y0, 0, 0, $b.Size); $g.Dispose(); $b }
function TopRow($frame, $base) {
    for ($y = 0; $y -lt $H; $y += 1) {
        $diff = 0
        for ($x = 0; $x -lt $W; $x += 8) {
            $a = $frame.GetPixel($x, $y); $c = $base.GetPixel($x, $y)
            if ([Math]::Abs($a.R - $c.R) + [Math]::Abs($a.G - $c.G) + [Math]::Abs($a.B - $c.B) -gt 40) { $diff++ }
        }
        if ($diff -ge 5) { return $Y0 + $y }
    }
    return -1
}

"before:  " + [Mc]::Taskbars()
[Mc]::SetCursorPos(960, 500) | Out-Null
$g = Start-Process (Join-Path $out 'guardian.exe') -PassThru
Start-Sleep 4
"running: " + [Mc]::Taskbars()
$halo = Get-Process halo -ErrorAction SilentlyContinue | Select-Object -First 1
"halo pid: $($halo.Id)"

# 2. reveal burst (dock is hidden: cursor has been away for > 500 ms)
$base = Grab
$sw = [Diagnostics.Stopwatch]::StartNew()
[Mc]::SetCursorPos(960, 1199) | Out-Null
$frames = @()
while ($sw.ElapsedMilliseconds -lt 400) { $frames += ,@($sw.ElapsedMilliseconds, (Grab)) }
"reveal: dock top edge (screen y) per captured frame:"
$tops = foreach ($f in $frames) { $t = TopRow $f[1] $base; "  {0,4} ms  y={1}" -f $f[0], $t; $f[1].Dispose() }
$tops
$base.Dispose()
[Mc]::SetCursorPos(960, 500) | Out-Null

# 3. explorer restart
if ($ExplorerRestart) {
    "restarting explorer..."
    Stop-Process -Name explorer -Force
    Start-Sleep 2
    if (-not (Get-Process explorer -ErrorAction SilentlyContinue)) { Start-Process explorer.exe }
    Start-Sleep 6
    "after explorer restart: " + [Mc]::Taskbars() + "  halo alive=" + [bool](Get-Process -Id $halo.Id -ErrorAction SilentlyContinue)
}

Start-Sleep 1
if ($halo) { [Mc]::PostMessage([Mc]::DockWindow([uint32]$halo.Id), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
Start-Sleep 3
"after:   " + [Mc]::Taskbars() + "  guardian alive=" + [bool](Get-Process -Id $g.Id -ErrorAction SilentlyContinue)
"--- guardian.log (last 8):"; Get-Content "$env:LOCALAPPDATA\Halo\guardian.log" -Tail 8
