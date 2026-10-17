# Screenshots the Notification Center without touching the live session. Launches an exe with HALO_TEST=1
# (this also makes it DRY-RUN: removals and launches are logged, never performed), HALO_TEST_NOHOOK=1, and
# toggles the panel with WM_APP+21 posted to ITS OWN dock window. Icon-source lines end up in "$Out.log".
#   tools/nc-shots.ps1 -Exe build/halo-nc.exe -Out build/nc -Steps hover,cal
param([string]$Exe, [string]$Out, [string]$Steps = 'open')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class T {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int l, t, r, b; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public static IntPtr Find(uint pid, string cls) { IntPtr r = IntPtr.Zero;
    EnumWindows((h,l)=>{ uint p; GetWindowThreadProcessId(h,out p); var sb=new StringBuilder(64); GetClassName(h,sb,64);
      if(p==pid && sb.ToString()==cls){ r=h; return false;} return true;}, IntPtr.Zero); return r; }
}
'@
[T]::SetProcessDPIAware() | Out-Null
$env:HALO_TEST = '1'; $env:HALO_TEST_NOHOOK = '1'; $env:HALO_TEST_CC_DRYRUN = '1'
$p = Start-Process $Exe -PassThru -RedirectStandardError "$Out.log" -WindowStyle Hidden
Start-Sleep -Seconds 2
$dock = [T]::Find($p.Id, 'HaloDockWindow')
function Shot($name) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap 520, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Right - 520, $b.Top, 0, 0, $bmp.Size)
  $bmp.Save("$Out-$name.png"); $g.Dispose(); $bmp.Dispose()
}
function Pt($x, $y) { [IntPtr](([int]$y -shl 16) -bor ([int]$x -band 0xFFFF)) }   # client px
try {
  [T]::PostMessage($dock, 0x8000 + 21, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 120; Shot 'early'
  Start-Sleep -Milliseconds 1500; Shot 'open'
  $nc = [T]::Find($p.Id, 'HaloNotifications'); "nc=$nc"
  $r = New-Object T+R; [T]::GetWindowRect($nc, [ref]$r) | Out-Null
  $s = [T]::GetDpiForWindow($nc) / 96.0; $pad = 16
  "scale=$s window=$($r.l),$($r.t) $($r.r - $r.l)x$($r.b - $r.t)"
  if ($Steps -match 'hover') {
    [T]::PostMessage($nc, 0x200, [IntPtr]0, (Pt (($pad + 120) * $s) (($pad + 100) * $s))) | Out-Null; Start-Sleep -Milliseconds 400; Shot 'hover'
    [T]::PostMessage($nc, 0x200, [IntPtr]0, (Pt (($pad + 337) * $s) (($pad + 48 + 20) * $s))) | Out-Null; Start-Sleep -Milliseconds 400; Shot 'hoverx'
    [T]::PostMessage($nc, 0x200, [IntPtr]0, (Pt (($pad + 330) * $s) (($pad + 19) * $s))) | Out-Null; Start-Sleep -Milliseconds 400; Shot 'hoverclear'
    [T]::PostMessage($nc, 0x201, [IntPtr]1, (Pt (($pad + 337) * $s) (($pad + 48 + 20) * $s))) | Out-Null; Start-Sleep -Milliseconds 700; Shot 'dismiss'
  }
  if ($Steps -match 'cal') {
    $r2 = New-Object T+R; [T]::GetWindowRect($nc, [ref]$r2) | Out-Null
    $totalH = ($r2.b - $r2.t) / $s - 2 * $pad; $calY = $totalH - 292
    $ny = $pad + $calY + 74 + 13
    [T]::PostMessage($nc, 0x200, [IntPtr]0, (Pt (($pad + 334) * $s) ($ny * $s))) | Out-Null; Start-Sleep -Milliseconds 300
    [T]::PostMessage($nc, 0x201, [IntPtr]1, (Pt (($pad + 334) * $s) ($ny * $s))) | Out-Null; Start-Sleep -Milliseconds 500; Shot 'calnext'
  }
  if ($Steps -match 'esc') {   # Esc must close it by itself (WM_KEYDOWN VK_ESCAPE)
    [T]::PostMessage($nc, 0x100, [IntPtr]0x1B, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 700
    "after Esc, window present: $([T]::Find($p.Id, 'HaloNotifications') -ne [IntPtr]::Zero)"
  }
  [T]::PostMessage($dock, 0x8000 + 21, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 120; Shot 'closing'
  Start-Sleep -Milliseconds 900; Shot 'closed'
} finally { if ($p.HasExited) { "EXITED code=$($p.ExitCode)" } else { Stop-Process -Id $p.Id -Force } }
Get-Content "$Out.log" -ErrorAction SilentlyContinue | Select-Object -Last 40
