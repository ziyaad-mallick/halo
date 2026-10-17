# Screenshots the combined panel (Control Center + notifications + calendar) without touching the live session.
# HALO_TEST=1 (dry run), HALO_TEST_NOHOOK=1, HALO_TEST_CC_DRYRUN=1, HALO_NOTIF_DRYRUN=1, HALO_TEST_POWER_DRYRUN=1.
#   tools/panel-shots.ps1 -Exe build/halo-one.exe -Tag panel
# Steps: open (always), toggle (close via the same message, reopen), esc (Esc to the CC window), focus (activate the
# notification window: nothing may close), power (click the Power action, shot the inline choice).
param([string]$Exe = '', [string]$Tag = 'panel', [string]$Steps = 'open,toggle,esc,focus', [double]$Scale = 1.25)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$Exe) { $Exe = Join-Path $repo 'build\halo-one.exe' }
$Out = Join-Path $repo "build\$Tag"
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class T {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
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
$env:HALO_TEST = '1'; $env:HALO_TEST_NOHOOK = '1'; $env:HALO_TEST_CC_DRYRUN = '1'; $env:HALO_NOTIF_DRYRUN = '1'; $env:HALO_TEST_POWER_DRYRUN = '1'
$p = Start-Process $Exe -PassThru -RedirectStandardError "$Out.log" -WindowStyle Hidden
Start-Sleep -Seconds 2
$dock = [T]::Find($p.Id, 'HaloDockWindow')
function Shot($name) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap 480, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Right - 480, $b.Top, 0, 0, $bmp.Size)
  $bmp.Save("$Out-$name.png"); $g.Dispose(); $bmp.Dispose()
}
function Toggle { [T]::PostMessage($dock, 0x8000 + 20, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
function State($label) {
  $cc = [T]::Find($p.Id, 'HaloControlCenter'); $nc = [T]::Find($p.Id, 'HaloNotifications')
  "$label : cc=$($cc -ne [IntPtr]::Zero) nc=$($nc -ne [IntPtr]::Zero)"
}
function Click($panel, $lx, $ly) {
  $x = [int]($lx * $Scale); $y = [int]($ly * $Scale)
  $l = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
  [T]::PostMessage($panel, 0x201, [IntPtr]1, $l) | Out-Null; Start-Sleep -Milliseconds 60
  [T]::PostMessage($panel, 0x202, [IntPtr]0, $l) | Out-Null; Start-Sleep -Milliseconds 300
}
try {
  State 'before'
  Toggle; Start-Sleep -Milliseconds 90; Shot 'early'; Start-Sleep -Milliseconds 1400
  State 'open'; Shot 'open'
  $cc = [T]::Find($p.Id, 'HaloControlCenter'); $nc = [T]::Find($p.Id, 'HaloNotifications')
  $r1 = New-Object T+RECT; $r2 = New-Object T+RECT
  [T]::GetWindowRect($cc, [ref]$r1) | Out-Null; [T]::GetWindowRect($nc, [ref]$r2) | Out-Null
  "cc window  $($r1.L),$($r1.T) $($r1.R-$r1.L)x$($r1.B-$r1.T)"; "nc window  $($r2.L),$($r2.T) $($r2.R-$r2.L)x$($r2.B-$r2.T)"
  "fg=$([T]::GetForegroundWindow()) cc=$cc nc=$nc"
  if ($Steps -match 'power') { Click $cc 161 ([int]($r1.B - $r1.T) / $Scale - 16 - 14 - 30); Start-Sleep -Milliseconds 500; Shot 'power' }
  if ($Steps -match 'toggle') {
    Toggle; Start-Sleep -Milliseconds 100; Shot 'closing'; Start-Sleep -Milliseconds 900; State 'after toggle-close'; Shot 'closed'
    Toggle; Start-Sleep -Milliseconds 1400; State 'reopened'
  }
  if ($Steps -match 'focus') {
    $nc = [T]::Find($p.Id, 'HaloNotifications')
    [T]::SetForegroundWindow($nc) | Out-Null; Start-Sleep -Milliseconds 600
    "fg after activating nc: $([T]::GetForegroundWindow()) (nc=$nc)"; State 'after focus moved to nc'; Shot 'focusnc'
  }
  if ($Steps -match 'esc') {
    $cc = [T]::Find($p.Id, 'HaloControlCenter')
    [T]::PostMessage($cc, 0x100, [IntPtr]0x1B, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 100; Shot 'esc-closing'; Start-Sleep -Milliseconds 800; State 'after Esc'
  }
} finally {
  [T]::PostMessage([T]::Find($p.Id, 'HaloDockWindow'), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Seconds 1
  if (!$p.HasExited) { Stop-Process -Id $p.Id -Force; 'killed test instance (only the test pid)' }
}
Get-Content "$Out.log"
