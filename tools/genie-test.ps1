# Drives halo.exe + a genietest window through minimize/restore/maximize/unmaximize
# and captures a screenshot burst of each. Refuses to run on the lock screen.
# Usage: tools/genie-test.ps1 -Out dir [-Steps min,restore,max,unmax] [-NoHalo] [-Frames 18]
param([string]$Out, [string]$Halo = (Join-Path (Split-Path $PSScriptRoot) 'build\halo.exe'),
      [string[]]$Steps = @('min','restore','max','unmax'), [switch]$NoHalo, [int]$Frames = 18)
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System; using System.Text; using System.Collections.Generic; using System.Diagnostics; using System.Drawing; using System.Runtime.InteropServices;
public static class G {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int sz);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  public static IntPtr FindByClassPid(string cls, uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { StringBuilder sb = new StringBuilder(64); GetClassName(h, sb, 64); uint p; GetWindowThreadProcessId(h, out p);
      if (sb.ToString() == cls && p == pid) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static IntPtr FindDialogOfPid(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); StringBuilder sb = new StringBuilder(64); GetClassName(h, sb, 64);
      if (p == pid && IsWindowVisible(h) && sb.ToString() == "GenieTest") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  public static string Burst(string dir, string tag, int n, int w, int h) {
    var shots = new List<Bitmap>(); var ts = new List<double>(); var sw = Stopwatch.StartNew();
    for (int i = 0; i < n; i++) {
      var b = new Bitmap(w, h); using (var g = Graphics.FromImage(b)) g.CopyFromScreen(0, 0, 0, 0, b.Size);
      shots.Add(b); ts.Add(sw.Elapsed.TotalMilliseconds);
    }
    var sb2 = new StringBuilder();
    for (int i = 0; i < n; i++) { shots[i].Save(dir + "\\" + tag + "-" + i.ToString("00") + ".png"); shots[i].Dispose(); sb2.Append(i + ":" + ts[i].ToString("F0") + "ms "); }
    return sb2.ToString();
  }
}
'@
[G]::SetProcessDPIAware() | Out-Null
$sb = New-Object System.Text.StringBuilder 200
[G]::GetWindowText([G]::GetForegroundWindow(), $sb, 200) | Out-Null
if ($sb.ToString() -match 'Lock Screen') { "SKIPPED: the PC is locked ($sb)"; return }
New-Item -ItemType Directory -Force $Out | Out-Null
$sw = [G]::GetSystemMetrics(0); $sh = [G]::GetSystemMetrics(1)
"screen ${sw}x${sh}"
$hp = $null; $haloHwnd = [IntPtr]::Zero
if (-not $NoHalo) {
  $hp = Start-Process $Halo -PassThru -RedirectStandardError (Join-Path $Out 'halo.log')
  Start-Sleep 2
  $haloHwnd = [G]::FindByClassPid('HaloDockWindow', [uint32]$hp.Id)
  "halo hwnd $haloHwnd"
}
$cm = Start-Process (Join-Path (Split-Path $PSScriptRoot) 'build\genietest.exe') -PassThru
Start-Sleep 2
$cm.Refresh(); $w = $cm.MainWindowHandle
"genietest pid $($cm.Id) hwnd $w"
function Rect($h) { $r = New-Object G+RECT; [G]::DwmGetWindowAttribute($h, 9, [ref]$r, 16) | Out-Null; "$($r.L),$($r.T),$($r.R),$($r.B)" }
function Ask($op) {
  $msg = [G]::RegisterWindowMessage('Halo.DockIconForWindow'); $r = [IntPtr]::Zero
  [G]::SendMessageTimeout($haloHwnd, $msg, $w, [IntPtr]$op, 2, 50, [ref]$r) | Out-Null
  $v = $r.ToInt64(); if ($v -eq 0) { return 'unknown' }
  $size = $v -band 0xFFFF; $cx = [int16](($v -shr 16) -band 0xFFFF); $cy = [int16](($v -shr 32) -band 0xFFFF)
  "raw=0x{0:X} size=$size centre=($cx,$cy) rect=($($cx - $size/2),$($cy - $size/2),$($cx + $size/2),$($cy + $size/2))" -f $v
}
if (-not $NoHalo) { "query: " + (Ask 3) }
"genietest rect at start: " + (Rect $w)
$cmds = @{ min = 0xF020; restore = 0xF120; max = 0xF030; unmax = 0xF120 }
foreach ($s in $Steps) {
  Start-Sleep -Milliseconds 700
  "== $s  before: iconic=$([G]::IsIconic($w)) zoomed=$([G]::IsZoomed($w)) rect=$(Rect $w)"
  [G]::PostMessage($w, 0x112, [IntPtr]$cmds[$s], [IntPtr]::Zero) | Out-Null
  $t = [G]::Burst($Out, $s, $Frames, $sw, $sh)
  "   frames: $t"
  Start-Sleep -Milliseconds 600
  "   after: iconic=$([G]::IsIconic($w)) zoomed=$([G]::IsZoomed($w)) rect=$(Rect $w)"
}
Start-Sleep -Milliseconds 500
Stop-Process -Id $cm.Id -ErrorAction SilentlyContinue
if ($hp) {
  [G]::PostMessage($haloHwnd, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep 1
  if (-not $hp.HasExited) { Stop-Process -Id $hp.Id }
  "halo log tail:"; Get-Content (Join-Path $Out 'halo.log') -Tail 6
}
