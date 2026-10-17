# Screenshots the Control Center without touching the live session or the system state.
# Launches an exe (default build/halo-cc.exe) with HALO_TEST=1 (opens via a posted message, no real
# keys), HALO_TEST_NOHOOK=1 (no keyboard hook, so it can't fight the live dock) and
# HALO_TEST_CC_DRYRUN=1 (toggles, sliders and media buttons are logged, not performed).
#   tools/cc-shots.ps1 -Steps open|click|slide|out [-Scale 1.5] [-Tag cc]
# Clicks are posted straight to the panel window in logical px (x Scale = client px).
# Output: build/<Tag>-*.png (top-right 460x900 of the primary screen) and build/<Tag>.log.
param([string]$Exe = '', [string]$Tag = 'cc', [string]$Steps = 'open', [double]$Scale = 1.0, [switch]$Real)
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
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out POINT p);
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
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
$env:HALO_TEST = '1'; $env:HALO_TEST_NOHOOK = '1'; if (-not $Real) { $env:HALO_TEST_CC_DRYRUN = '1' } else { Remove-Item Env:HALO_TEST_CC_DRYRUN -ErrorAction SilentlyContinue }
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
  # Coordinates are client px of the panel window = panel logical + motion::kPad (16) of margin.
  if ($Steps -match 'click') {
    Click $panel 60 60; Click $panel 221 60; Click $panel 62 134; Click $panel 124 134; Click $panel 186 134; Click $panel 248 134; Click $panel 310 134
    Shot 'clicked'
  }
  if ($Steps -match 'slide') { Click $panel 150 224; Click $panel 100 302; Shot 'slid' }
  if ($Steps -match 'wifi') { Click $panel 116 60; Start-Sleep -Seconds 3; Shot 'wifi' }
  if ($Steps -match 'bt') { Click $panel 286 60; Start-Sleep -Seconds 4; Shot 'bt' }
  if ($Steps -match 'out') { Click $panel 276 271; Shot 'outs' }
  if ($Steps -match 'chord') { Click $panel 63 465; Click $panel 139 465; Click $panel 215 465; Click $panel 300 465 }
  if ($Steps -match 'night') {   # -Real only: toggles Night light twice and prints the CloudStore blob each time
    $nk = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\CloudStore\Store\DefaultAccount\Current\default$windows.data.bluelightreduction.bluelightreductionstate\windows.data.bluelightreduction.bluelightreductionstate'
    function Blob { ((Get-ItemProperty $nk).Data | ForEach-Object { $_.ToString('X2') }) -join ' ' }
    "night before : $(Blob)"
    Click $panel 186 134; Start-Sleep -Milliseconds 1500; Shot 'night1'; "night toggle 1: $(Blob)"
    Click $panel 186 134; Start-Sleep -Milliseconds 1500; Shot 'night2'; "night toggle 2: $(Blob)"
  }
  if ($Steps -match 'nightone') { Click $panel 186 134; Start-Sleep -Milliseconds 1500 }   # -Real only: one Night light toggle
  if ($Steps -match 'darkmode') {   # -Real only
    $tk = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
    function Theme { $v = Get-ItemProperty $tk; "Apps=$($v.AppsUseLightTheme) System=$($v.SystemUsesLightTheme)" }
    "dark before : $(Theme)"
    Click $panel 248 134; Start-Sleep -Milliseconds 1500; Shot 'dark1'; "dark toggle 1: $(Theme)"
    Click $panel 248 134; Start-Sleep -Milliseconds 1500; Shot 'dark2'; "dark toggle 2: $(Theme)"
  }
  if ($Steps -match 'hover') {   # hover the Night light circle, press it without releasing, then hover the Cast tile
    # WM_MOUSELEAVE fires at once for a synthetic move unless the real cursor is over the window, so park it there.
    $r = New-Object T+RECT; [T]::GetWindowRect($panel, [ref]$r) | Out-Null; $cp = New-Object T+POINT; [T]::GetCursorPos([ref]$cp) | Out-Null
    function At($lx, $ly) { [T]::SetCursorPos($r.L + [int]($lx * $Scale), $r.T + [int]($ly * $Scale)) | Out-Null; $l = [IntPtr](([int]($ly * $Scale) -shl 16) -bor [int]($lx * $Scale)); $l }
    $l = At 186 134
    [T]::PostMessage($panel, 0x200, [IntPtr]0, $l) | Out-Null; Start-Sleep -Milliseconds 400; Shot 'hover'
    [T]::PostMessage($panel, 0x201, [IntPtr]1, $l) | Out-Null; Start-Sleep -Milliseconds 250; Shot 'press'
    [T]::PostMessage($panel, 0x202, [IntPtr]0, $l) | Out-Null; Start-Sleep -Milliseconds 400
    $l2 = At 139 465
    [T]::PostMessage($panel, 0x200, [IntPtr]0, $l2) | Out-Null; Start-Sleep -Milliseconds 400; Shot 'hover2'
    [T]::SetCursorPos($cp.X, $cp.Y) | Out-Null
  }
  if ($Steps -match 'anim') {   # frames of the close bounce, then of the open bounce
    [T]::PostMessage($dock, 0x8000 + 20, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    foreach ($ms in 40, 50, 60, 70) { Start-Sleep -Milliseconds $ms; Shot "close$ms" }
    Start-Sleep -Milliseconds 600
    [T]::PostMessage($dock, 0x8000 + 20, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    foreach ($ms in 30, 40, 60, 80) { Start-Sleep -Milliseconds $ms; Shot "open$ms" }
    Start-Sleep -Milliseconds 600
  }
  if ($Steps -match 'reopen') {   # open/close cycles in one process: the latency log shows cold vs warm opens
    1..5 | ForEach-Object {
      [T]::PostMessage($dock, 0x8000 + 20, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 700
      [T]::PostMessage($dock, 0x8000 + 20, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 700
    }
  }
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
