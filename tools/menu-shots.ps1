# Exercise and screenshot the dock's right-click menu without the real mouse or the live pin list.
# Starts a test copy (build/halo-menu.exe, override with -Exe) with HALO_TEST=1, HALO_TEST_NOHOOK=1,
# HALO_TEST_MENU_DRYRUN=1 (Quit / Remove from Dock / Show in Explorer only log) and HALO_PINS_FILE pointing
# at a temp copy of the pin list, and drives it with posted messages:
#   WM_APP+31 to the dock window: wParam = dock item, lParam 0 = open its menu, 1 = close,
#     100+r = hover row r, 200+r = choose row r
# The menu logs every row and its screen rect ("[halo] menu: ..."), so on a locked PC (screenshots
# are blank then) the log is the evidence. Grabs the bottom -Height px of the primary screen after each step.
#   tools/menu-shots.ps1 [-Tag menu] [-Height 400] [-Exe build/halo-menu.exe] [-Pinned 0] [-Running <item>]
# -Pinned: dock item to open as a pinned app (default 0). -Running: item to open as a running app
# (default: the first item the test copy reports as running).
# Output: build/<Tag>-*.png and build/<Tag>.log (the app's stderr), whose menu lines are printed at the end.
param([string]$Tag = 'menu', [int]$Height = 400, [string]$Exe = '', [int]$Pinned = 0, [int]$Running = -1)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
if (-not $Exe) { $Exe = Join-Path $out 'halo-menu.exe' }
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Ms {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
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
[Ms]::SetProcessDPIAware() | Out-Null
$W = [Ms]::GetSystemMetrics(0); $H = [Ms]::GetSystemMetrics(1)
$sb = New-Object System.Text.StringBuilder 200
[Ms]::GetWindowText([Ms]::GetForegroundWindow(), $sb, 200) | Out-Null
$locked = $sb.ToString() -match 'Lock Screen'
if ($locked) { "NOTE: the PC is locked ($sb): screenshots are skipped, the log is the evidence." }

# A temp pin list: the live %LOCALAPPDATA%\Halo\pins.txt is never touched. Seeded from the taskbar on first start.
$pins = Join-Path $out "$Tag-pins.txt"
Remove-Item $pins -ErrorAction SilentlyContinue
$log = Join-Path $out "$Tag.log"
$env:HALO_TEST = '1'; $env:HALO_TEST_NOHOOK = '1'; $env:HALO_TEST_MENU_DRYRUN = '1'; $env:HALO_PINS_FILE = $pins
$p = Start-Process -FilePath $Exe -PassThru -RedirectStandardError $log -RedirectStandardOutput (Join-Path $out "$Tag.out")
try {
    $dock = [IntPtr]::Zero
    for ($i = 0; $i -lt 40 -and $dock -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 150; $dock = [Ms]::Find($p.Id, 'HaloDockWindow') }
    if ($dock -eq [IntPtr]::Zero) { throw "no dock window; see $log" }
    Start-Sleep -Milliseconds 1500
    function Shot($name) {
        if ($locked) { return }
        $b = New-Object System.Drawing.Bitmap $W, $Height
        $g = [System.Drawing.Graphics]::FromImage($b)
        $g.CopyFromScreen(0, $H - $Height, 0, 0, $b.Size); $g.Dispose()
        $path = Join-Path $out "$Tag-$name.png"; $b.Save($path); $b.Dispose(); "shot $path"
    }
    function Menu($item, $op) { [Ms]::PostMessage($dock, 0x801F, [IntPtr]$item, [IntPtr]$op) | Out-Null }
    if ($Running -lt 0) {
        $line = Get-Content $log | Where-Object { $_ -match 'test icon \d+ .* running' } | Select-Object -First 1
        if ($line -and $line -match 'test icon (\d+) ') { $Running = [int]$Matches[1] }
    }
    "pinned item = $Pinned, running item = $Running"

    Menu $Pinned 0; Start-Sleep -Milliseconds 600; Shot 'pinned'
    Menu 0 101; Start-Sleep -Milliseconds 300; Shot 'pinned-hover'
    Menu 0 1; Start-Sleep -Milliseconds 400; Shot 'closed'
    if ($Running -ge 0) {
        Menu $Running 0; Start-Sleep -Milliseconds 600; Shot 'running'
        Menu 0 100; Start-Sleep -Milliseconds 300; Shot 'running-hover'
        $rows = @(Get-Content $log | Where-Object { $_ -match '\[halo\] menu: row ' })
        $quit = ($rows | Select-String "'Quit'" | Select-Object -Last 1)
        if ($quit -and $quit.Line -match 'menu: row (\d+) ') { Menu 0 (200 + [int]$Matches[1]); Start-Sleep -Milliseconds 400 }   # dry run: logs only
        Menu $Running 0; Start-Sleep -Milliseconds 400
    }
    Menu $Pinned 0; Start-Sleep -Milliseconds 400
    $rows = @(Get-Content $log | Where-Object { $_ -match '\[halo\] menu: row ' })
    $rm = ($rows | Select-String "'Remove from Dock'" | Select-Object -Last 1)
    if ($rm -and $rm.Line -match 'menu: row (\d+) ') { Menu 0 (200 + [int]$Matches[1]); Start-Sleep -Milliseconds 400 }       # dry run: logs only
    # Esc as the keyboard hook would post it (the test copy runs without the hook).
    Menu $Pinned 0; Start-Sleep -Milliseconds 400
    [Ms]::PostMessage($dock, [Ms]::RegisterWindowMessage('Halo.WinKeyEscape'), [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 400; Shot 'after-esc'

    [Ms]::PostMessage($dock, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
    $p.WaitForExit(3000) | Out-Null
} finally {
    if (-not $p.HasExited) { $p.Kill() }
}
Get-Content $log | Select-String 'menu:|pins:'
