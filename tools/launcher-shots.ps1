# Screenshot and exercise the Launcher panel (Start) without the real keyboard, mouse or power state.
# Starts build/halo-launcher.exe (override with -Exe) with HALO_TEST=1, HALO_TEST_NOHOOK=1 and HALO_TEST_POWER_DRYRUN=1
# (power actions only log) and drives it with posted messages:
#   WM_APP+8 to the dock window: wParam 0 = open the panel, 1 = close, 2 = toggle the Power menu,
#     3..7 = run Power item 0..4 (logs only), 9 = pretend the panel lost activation
#   WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_LBUTTONUP / WM_CHAR / WM_KEYDOWN to the panel window, at the points the app logs
# Grabs the panel window with PrintWindow after each step (works on a locked session, but the glass backdrop blur is not captured: the panel shows its tint over grey).
#   tools/launcher-shots.ps1 [-Tag launcher] [-Exe build/halo-launcher.exe] [-Recent <recent.txt>]
# Output: build/<Tag>-*.png and build/<Tag>.log (the app's stderr), which is printed at the end.
param([string]$Tag = 'launcher', [int]$Height = 700, [string]$Exe = '', [string]$Recent = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
if (-not $Exe) { $Exe = Join-Path $out 'halo-launcher.exe' }
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Ls {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  public struct RECT { public int l, t, r, b; }
  public static void Grab(IntPtr h, string path) { RECT r; GetWindowRect(h, out r); var b = new System.Drawing.Bitmap(r.r - r.l, r.b - r.t); var g = System.Drawing.Graphics.FromImage(b); var dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); g.Dispose(); b.Save(path); }
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
$env:HALO_TEST = '1'; $env:HALO_TEST_NOHOOK = '1'; $env:HALO_TEST_POWER_DRYRUN = '1'; $env:HALO_TEST_STAY = '1'
$p = Start-Process -FilePath $Exe -PassThru -RedirectStandardError $log -RedirectStandardOutput (Join-Path $out "$Tag.out")
try {
    $dock = [IntPtr]::Zero
    for ($i = 0; $i -lt 40 -and $dock -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 150; $dock = [Ls]::Find($p.Id, 'HaloDockWindow') }
    if ($dock -eq [IntPtr]::Zero) { throw "no dock window; see $log" }
    Start-Sleep -Milliseconds 800
    function Shot($name) {
        Cmd 20; Start-Sleep -Milliseconds 150   # snap the hover plate/icon to their end state (PrintWindow does not see animations)
        $path = Join-Path $out "$Tag-$name.png"; [Ls]::Grab($panel, $path); "shot $path"
    }
    function Cmd($w) { [Ls]::PostMessage($dock, 0x8008, [IntPtr]$w, [IntPtr]::Zero) | Out-Null }
    function Point($name) {   # where the app says things are, in the panel window's client px
        $line = (Get-Content $log | Where-Object { $_ -match 'test points' } | Select-Object -Last 1)
        if ($line -notmatch "$name=(\d+),(\d+)") { throw "no test point $name in $log" }
        [IntPtr](([int]$Matches[2] -shl 16) -bor [int]$Matches[1])
    }
    function Mouse($msg, $point) { [Ls]::PostMessage($panel, $msg, [IntPtr]($(if ($msg -eq 0x201) { 1 } else { 0 })), $point) | Out-Null }
    function TypeText($text) { foreach ($c in $text.ToCharArray()) { [Ls]::PostMessage($panel, 0x102, [IntPtr][int]$c, [IntPtr]::Zero) | Out-Null; Start-Sleep -Milliseconds 25 } }
    function Key($vk) { [Ls]::PostMessage($panel, 0x100, [IntPtr]$vk, [IntPtr]::Zero) | Out-Null }

    Cmd 0; Start-Sleep -Milliseconds 900
    $panel = [Ls]::Find($p.Id, 'HaloLauncherWindow')
    if ($panel -eq [IntPtr]::Zero) { throw 'no panel window' }
    Shot 'empty'
    Mouse 0x200 (Point 'app0');  Start-Sleep -Milliseconds 500; Shot 'hover-app'
    Mouse 0x200 (Point 'file7'); Start-Sleep -Milliseconds 500; Shot 'hover-file'
    TypeText 'chr'; Start-Sleep -Milliseconds 500; Shot 'search-chr'
    Key 0x28; Start-Sleep -Milliseconds 400; Shot 'search-chr-down'
    TypeText 'x'; Start-Sleep -Milliseconds 400; Shot 'search-none'
    TypeText ([string][char]8); TypeText ([string][char]8); TypeText ([string][char]8); TypeText ([string][char]8); Start-Sleep -Milliseconds 300; Shot 'search-cleared'
    TypeText 'set'; Start-Sleep -Milliseconds 300
    Key 0x1B; Start-Sleep -Milliseconds 300; "after first Esc (text cleared, panel still open): " + (Get-Content $log | Select-String 'hiding').Count + " hiding lines (want 0)"
    Key 0x1B; Start-Sleep -Milliseconds 600; "after second Esc: " + (Get-Content $log | Select-String 'hiding').Count + " hiding lines (want 1)"
    Cmd 0; Start-Sleep -Milliseconds 700
    $pw = Point 'power'; Mouse 0x200 $pw; Mouse 0x201 $pw; Mouse 0x202 $pw; Start-Sleep -Milliseconds 500; Shot 'menu'
    Mouse 0x200 (Point 'menu2'); Start-Sleep -Milliseconds 400
    $m2 = Point 'menu2'; Mouse 0x201 $m2; Mouse 0x202 $m2; Start-Sleep -Milliseconds 700   # dry run: logs only
    Cmd 0; Start-Sleep -Milliseconds 700; Cmd 9; Start-Sleep -Milliseconds 500
    "after a pretend click elsewhere: " + (Get-Content $log | Select-String 'hiding').Count + " hiding lines"
    [Ls]::PostMessage($dock, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null   # WM_CLOSE
    $p.WaitForExit(3000) | Out-Null
} finally {
    if (-not $p.HasExited) { $p.Kill() }
}
Get-Content $log | Select-String 'launcher'
