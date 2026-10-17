# Screenshots the Control Center without touching the live session or the system state.
# Launches an exe (default build/halo-cc.exe) with HALO_TEST=1 (opens via a posted message, no real
# keys), HALO_TEST_NOHOOK=1 (no keyboard hook, so it can't fight the live dock) and
# HALO_TEST_CC_DRYRUN=1 (toggles, sliders and media buttons are logged, not performed).
#   tools/cc-shots.ps1 -Steps open|click|slide|out [-Scale 1.5] [-Tag cc]
# Clicks are posted straight to the panel window in logical px (x Scale = client px).
# Output: build/<Tag>-*.png (top-right 460x900 of the primary screen) and build/<Tag>.log.
param([string]$Exe = '', [string]$Tag = 'cc', [string]$Steps = 'open', [double]$Scale = 1.0)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$Exe) { $Exe = Join-Path $repo 'build\halo-cc.exe' }
$Out = Join-Path $repo "build\$Tag"
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class T {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
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
  $bmp = New-Object System.Drawing.Bitmap 460, 900
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Right - 460, $b.Top, 0, 0, $bmp.Size)
  $bmp.Save("$Out-$name.png"); $g.Dispose(); $bmp.Dispose()
}
function Click($panel, $lx, $ly) {
  $x = [int]($lx * $Scale); $y = [int]($ly * $Scale)
  $l = [IntPtr](($y -shl 16) -bor ($x -band 0xFFFF))
  [T]::PostMessage($panel, 0x201, [IntPtr]1, $l) | Out-Null; Start-Sleep -Milliseconds 60
  [T]::PostMessage($panel, 0x202, [IntPtr]0, $l) | Out-Null; Start-Sleep -Milliseconds 300
}
try {
  $mem0 = (Get-Process -Id $p.Id).PrivateMemorySize64 / 1MB
  if ($Steps -match 'hot') {
    $hz = [T]::Find($p.Id, 'HaloControlCenterHotZone'); "hotzone=$hz"
    [T]::PostMessage($hz, 0x113, [IntPtr]1, [IntPtr]::Zero) | Out-Null   # the dwell timer firing
  } else {
  [T]::PostMessage($dock, 0x8000 + 20, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  }
  Start-Sleep -Milliseconds 1500
  $panel = [T]::Find($p.Id, 'HaloControlCenter')
  "panel=$panel foreground=$([T]::GetForegroundWindow())"
  Shot 'open'
  if ($Steps -match 'click') {
    Click $panel 80 110; Click $panel 80 144; Click $panel 80 178; Click $panel 80 212; Click $panel 250 120; Click $panel 216 200; Click $panel 40 281; Click $panel 107 281; Click $panel 280 58
    Shot 'clicked'
  }
  if ($Steps -match 'slide') { Click $panel 150 360; Click $panel 100 438; Shot 'slid' }
  if ($Steps -match 'wifi') { Click $panel 136 110; Start-Sleep -Seconds 3; Shot 'wifi'; Start-Sleep -Seconds 6; Shot 'wifi2' }
  if ($Steps -match 'bt') { Click $panel 136 110; Click $panel 136 144; Start-Sleep -Seconds 4; Shot 'bt' }
  if ($Steps -match 'out') { Click $panel 250 422; Shot 'outs' }
  if ($Steps -match 'chord') { Click $panel 174 281; Click $panel 240 281; Click $panel 300 281 }
  $mem1 = (Get-Process -Id $p.Id).PrivateMemorySize64 / 1MB
  if ($Steps -match 'esc') {
    [T]::PostMessage($panel, 0x100, [IntPtr]0x1B, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 700
    $gone = [T]::Find($p.Id, 'HaloControlCenter')
    $mem2 = (Get-Process -Id $p.Id).PrivateMemorySize64 / 1MB
    "after Esc: panel window=$gone ; private MB closed=$([int]$mem0) open=$([int]$mem1) after-close=$([int]$mem2)"
  }
} finally {
  [T]::PostMessage([T]::Find($p.Id, 'HaloDockWindow'), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # WM_CLOSE
  Start-Sleep -Seconds 1
  if (!$p.HasExited) { 'still running (not killed)' }
}
Get-Content "$Out.log"
