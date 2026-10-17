# Screenshot and exercise the Launcher panel without the real keyboard, mouse or power state.
# Starts build/halo-launcher.exe (override with -Exe) with HALO_TEST=1 and HALO_TEST_POWER_DRYRUN=1 (power
# actions only log) and drives it with posted messages:
#   WM_APP+8 to the dock window: wParam 0 = open the panel with no Command Palette, 1 = close,
#     2 = toggle the Power menu, 3..7 = run Power item 0..4 (logs only)
#   WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_LBUTTONUP to the panel window, at the points the app logs
#   the registered "Halo.WinKeyEscape" message, as the keyboard hook would post it
# Foreground changes are simulated (WM_APP+8, wParam 9 and 10), not real; see the comment where they are used.
# Grabs the bottom -Height px of the primary screen after each step.
#   tools/launcher-shots.ps1 [-Tag launcher] [-Height 700] [-Exe build/halo-launcher.exe] [-Recent <recent.txt>]
# Output: build/<Tag>-*.png and build/<Tag>.log (the app's stderr), which is printed at the end.
param([string]$Tag = 'launcher', [int]$Height = 700, [string]$Exe = '', [string]$Recent = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
if (-not $Exe) { $Exe = Join-Path $out 'halo-launcher.exe' }
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Ls {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string s);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  public static IntPtr Find(uint target, string cls) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid);
      var sb = new StringBuilder(64); GetClassName(h, sb, 64);
      if (pid == target && sb.ToString() == cls) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
'@
[Ls]::SetProcessDPIAware() | Out-Null
$W = [Ls]::GetSystemMetrics(0); $H = [Ls]::GetSystemMetrics(1)

$log = Join-Path $out "$Tag.log"
if ($Recent) { $env:HALO_TEST_RECENT = $Recent }   # a stand-in recent.txt, so the real list is left alone
$env:HALO_TEST = '1'; $env:HALO_TEST_POWER_DRYRUN = '1'
$p = Start-Process -FilePath $Exe -PassThru -RedirectStandardError $log -RedirectStandardOutput (Join-Path $out "$Tag.out")
try {
    $dock = [IntPtr]::Zero
    for ($i = 0; $i -lt 40 -and $dock -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 150; $dock = [Ls]::Find($p.Id, 'HaloDockWindow') }
    if ($dock -eq [IntPtr]::Zero) { throw "no dock window; see $log" }
    Start-Sleep -Milliseconds 800
    function Shot($name) {
        $b = New-Object System.Drawing.Bitmap $W, $Height
        $g = [System.Drawing.Graphics]::FromImage($b)
        $g.CopyFromScreen(0, $H - $Height, 0, 0, $b.Size); $g.Dispose()
        $path = Join-Path $out "$Tag-$name.png"; $b.Save($path); $b.Dispose(); "shot $path"
    }
    function Cmd($w) { [Ls]::PostMessage($dock, 0x8008, [IntPtr]$w, [IntPtr]::Zero) | Out-Null }
    # Where the app says things are, in the panel window's client px.
    function Point($name) {
        $line = (Get-Content $log | Where-Object { $_ -match 'test points' } | Select-Object -Last 1)
        if ($line -notmatch "$name=(\d+),(\d+)") { throw "no test point $name in $log" }
        [IntPtr](([int]$Matches[2] -shl 16) -bor [int]$Matches[1])
    }
    function Mouse($panel, $msg, $point) { [Ls]::PostMessage($panel, $msg, [IntPtr]($(if ($msg -eq 0x201) { 1 } else { 0 })), $point) | Out-Null }

    Cmd 0; Start-Sleep -Milliseconds 900
    $panel = [Ls]::Find($p.Id, 'HaloLauncherWindow')
    if ($panel -eq [IntPtr]::Zero) { throw 'no panel window' }
    Shot 'open'
    Mouse $panel 0x200 (Point 'app0');  Start-Sleep -Milliseconds 400; Shot 'hover-app'
    Mouse $panel 0x200 (Point 'power'); Start-Sleep -Milliseconds 400; Shot 'hover-power'
    $pw = Point 'power'; Mouse $panel 0x201 $pw; Mouse $panel 0x202 $pw; Start-Sleep -Milliseconds 500; Shot 'menu'
    Mouse $panel 0x200 (Point 'menu2'); Start-Sleep -Milliseconds 400; Shot 'menu-hover'
    $m2 = Point 'menu2'; Mouse $panel 0x201 $m2; Mouse $panel 0x202 $m2; Start-Sleep -Milliseconds 700; Shot 'after-menu-click'   # dry run: logs only
    foreach ($n in 3..7) { Cmd 0; Start-Sleep -Milliseconds 300; Cmd $n; Start-Sleep -Milliseconds 100; Cmd 1; Start-Sleep -Milliseconds 300 }

    # Esc, as the keyboard hook posts it.
    Cmd 0; Start-Sleep -Milliseconds 600
    [Ls]::PostMessage($dock, [Ls]::RegisterWindowMessage('Halo.WinKeyEscape'), [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 600; Shot 'after-esc'

    # Dismissal by foreground change. The real hook cannot be driven from here (a background process may not
    # take the foreground), so the panel is told what the hook would tell it: 9 = the desktop took the
    # foreground, 10 = Command Palette's process did. Mode 100 opens it as the Win key does.
    function Moved { @(Get-Content $log | Where-Object { $_ -match 'foreground moved' }).Count }
    $n0 = Moved
    Cmd 100; Start-Sleep -Milliseconds 100; Cmd 9; Start-Sleep -Milliseconds 400
    "step 1, desktop focus inside the 800 ms grace, palette not seen yet: moved events " + ((Moved) - $n0) + " (want 0)"; Shot 'grace'
    Cmd 10; Start-Sleep -Milliseconds 100; Cmd 9; Start-Sleep -Milliseconds 700
    "step 2, palette took focus, then the desktop: moved events " + ((Moved) - $n0) + " (want 1)"; Shot 'palette-then-desktop'
    Cmd 100; Start-Sleep -Milliseconds 1200; Cmd 9; Start-Sleep -Milliseconds 700
    "step 3, palette never appeared, desktop focus after the grace: moved events " + ((Moved) - $n0) + " (want 2)"
    Cmd 100; Start-Sleep -Milliseconds 100; Cmd 10; Start-Sleep -Milliseconds 1200; Cmd 10; Start-Sleep -Milliseconds 500
    "step 4, palette focus only: moved events " + ((Moved) - $n0) + " (want 2, panel still up)"; Shot 'palette-stays'

    [Ls]::PostMessage($dock, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
    $p.WaitForExit(3000) | Out-Null
} finally {
    if (-not $p.HasExited) { $p.Kill() }
}
Get-Content $log | Select-String 'launcher'
