# Screenshot the dock without touching the real mouse. Launches build/halo.exe with
# HALO_TEST=1 (starts shown, no auto-hide), then drives it with posted messages:
# WM_APP+2 (wParam = pointer x) simulates a pointer move, WM_APP+3 a leave, and
# posted WM_LBUTTONDOWN/UP simulate clicks. Refuses to run on the lock screen.
#   tools/shots.ps1 -Scenario rest|hover|open|close|press|bounce [-Tag name] [-X 960]
#                   [-Icon 0] [-App notepad.exe]
# rest/hover: one PNG each. open/close: a burst of frames while -App starts/exits.
# press: a magnified frame with -Icon held down. bounce: a burst after clicking -Icon
# (-Prelaunch <exe> -PrelaunchMs N starts that app N ms before the click).
# Output: build/<Tag>-*.png and build/<Tag>-sheet.png (a stacked contact sheet of a burst).
param([string]$Scenario = 'hover', [string]$Tag = 'shots', [int]$X = 960, [int]$Icon = 0,
      [string]$App = 'notepad.exe', [int]$Frames = 16, [int]$IntervalMs = 100,
      [string]$Prelaunch = '', [int]$PrelaunchMs = 0)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Sh {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  // The dock window of a process (FindWindow by class alone misses it).
  public static IntPtr DockWindow(uint target) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid);
      var sb = new StringBuilder(64); GetClassName(h, sb, 64);
      if (pid == target && sb.ToString() == "HaloDockWindow") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
'@
[Sh]::SetProcessDPIAware() | Out-Null
$sb = New-Object System.Text.StringBuilder 200
[Sh]::GetWindowText([Sh]::GetForegroundWindow(), $sb, 200) | Out-Null
if ($sb.ToString() -match 'Lock Screen') { "SKIPPED: the PC is locked ($sb)"; return }

$script:CropX = 700               # left edge of the contact sheet's crop
$top = 1000                       # screenshots cover the bottom 200 px
function Shot($name) {
    $b = New-Object System.Drawing.Bitmap 1920, 200
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.CopyFromScreen(0, $top, 0, 0, $b.Size)
    $p = Join-Path $out "$Tag-$name.png"; $b.Save($p); $g.Dispose(); $b.Dispose(); $p
}
# Frames as fast as the screen grab allows for $ms, calling $action once after $afterMs.
function Burst($ms, $afterMs, $action) {
    $frames = @(); $sw = [Diagnostics.Stopwatch]::StartNew(); $fired = $false
    while ($sw.ElapsedMilliseconds -lt $ms) {
        if (-not $fired -and $sw.ElapsedMilliseconds -ge $afterMs) { & $action; $fired = $true }
        $b = New-Object System.Drawing.Bitmap 1920, 200
        $g = [System.Drawing.Graphics]::FromImage($b)
        $g.CopyFromScreen(0, $top, 0, 0, $b.Size); $g.Dispose()
        $frames += [pscustomobject]@{ T = $sw.ElapsedMilliseconds; Bmp = $b }
        $left = $IntervalMs - ($sw.ElapsedMilliseconds - $frames[-1].T)
        if ($left -gt 0) { Start-Sleep -Milliseconds $left }
    }
    $frames
}
# Stacks the middle 1000 px of each frame (labelled with its time) into one image.
# Keeps the frames that differ from the last kept one (sampled pixels), at most $max.
function Distinct($frames, $max) {
    $kept = @(); $last = $null
    foreach ($f in $frames) {
        $d = 1e9
        if ($last) { $d = 0; for ($y = 110; $y -lt 200; $y += 5) { for ($x = $script:CropX; $x -lt $script:CropX + 900; $x += 5) {
            $a = $f.Bmp.GetPixel($x, $y); $b = $last.Bmp.GetPixel($x, $y)
            $d += [math]::Abs($a.R - $b.R) + [math]::Abs($a.G - $b.G) + [math]::Abs($a.B - $b.B) } } }
        if ($d -gt 3000) { $kept += $f; $last = $f }
    }
    if ($kept.Count -gt $max) { $kept = $kept[0..($max - 1)] }
    $kept
}
function Sheet($frames, $name) {
    $frames = Distinct $frames 30
    $w = 900; $h = 90; $x0 = $script:CropX
    $sheet = New-Object System.Drawing.Bitmap $w, ($h * $frames.Count)
    $g = [System.Drawing.Graphics]::FromImage($sheet); $g.Clear([System.Drawing.Color]::FromArgb(40, 40, 40))
    $font = New-Object System.Drawing.Font 'Consolas', 9
    for ($i = 0; $i -lt $frames.Count; $i++) {
        $g.DrawImage($frames[$i].Bmp, [System.Drawing.Rectangle]::new(0, $i * $h, $w, $h),
                     [System.Drawing.Rectangle]::new($x0, 200 - $h, $w, $h), [System.Drawing.GraphicsUnit]::Pixel)
        $g.DrawString("$($frames[$i].T) ms", $font, [System.Drawing.Brushes]::Yellow, 4, $i * $h + 2)
        $frames[$i].Bmp.Save((Join-Path $out ("{0}-{1}-f{2:D2}.png" -f $Tag, $name, $i)))
        $frames[$i].Bmp.Dispose()
    }
    $p = Join-Path $out "$Tag-$name-sheet.png"; $sheet.Save($p); $g.Dispose(); $sheet.Dispose(); $p
}

$env:HALO_TEST = '1'
$p = Start-Process (Join-Path $out 'halo.exe') -PassThru -RedirectStandardError (Join-Path $out "$Tag.log")
Remove-Item Env:HALO_TEST
Start-Sleep -Seconds 3
$dock = [Sh]::DockWindow([uint32]$p.Id)
if ($dock -eq [IntPtr]::Zero) { Stop-Process -Id $p.Id; throw 'dock window not found' }
function PMove($x) { [Sh]::PostMessage($dock, 0x8002, [IntPtr]$x, [IntPtr]::Zero) | Out-Null }
function Leave { [Sh]::PostMessage($dock, 0x8003, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
function Mouse($msg, $x, $y) { [Sh]::PostMessage($dock, $msg, [IntPtr]($(if ($msg -eq 0x201) { 1 } else { 0 })), [IntPtr](($y -shl 16) -bor $x)) | Out-Null }
# Centre of icon $i at rest: 47.5 px pitch at 125%, a 12.5 px divider slot before the unpinned icons, centred on 960.
function IconX($i) {
    $log = Get-Content (Join-Path $out "$Tag.log") -Raw
    $n = [int]([regex]::Match($log, '(\d+) icons').Groups[1].Value)
    $pinned = [int]([regex]::Match($log, '(\d+) apps').Groups[1].Value)
    960 - ($n * 47.5 + 12.5) / 2 + 47.5 * ($i + 0.5) + $(if ($i -ge $pinned) { 12.5 } else { 0 })
}
try {
    switch ($Scenario) {
        'rest'  { Shot 'rest' }
        'hover' { Shot 'rest'; PMove $X; Start-Sleep -Milliseconds 600; Shot 'hover'; Leave }
        'open'  { Shot 'before'
                  $f = Burst 2600 400 { $script:started = Start-Process $App -PassThru }
                  Sheet $f 'open'; Start-Sleep -Seconds 1; Shot 'after'
                  Get-Process | Where-Object { $_.Id -eq $started.Id } | ForEach-Object { $_.CloseMainWindow() | Out-Null } }
        'close' { $started = Start-Process $App -PassThru; Start-Sleep -Seconds 4; Shot 'before'
                  $f = Burst 2000 300 { $started.CloseMainWindow() | Out-Null }
                  if (-not $started.HasExited) { Stop-Process -Id $started.Id -ErrorAction SilentlyContinue }
                  Sheet $f 'close'; Shot 'after' }
        'press' { $ix = [int](IconX $Icon); PMove $ix; Start-Sleep -Milliseconds 600
                  Shot 'hover'
                  Mouse 0x201 $ix 85; Start-Sleep -Milliseconds 300; Shot 'pressed'
                  Mouse 0x202 0 0; Start-Sleep -Milliseconds 400; Shot 'released'; Leave }
        'bounce' { $ix = [int](IconX $Icon); $script:CropX = [math]::Max(0, $ix - 150)
                  # -Prelaunch starts the app (a path) that long before the click: a slow starter then
                  # shows its window early in the bounce, as a fast one would.
                  $f = Burst 3000 200 { if ($Prelaunch) { [Diagnostics.Process]::Start($Prelaunch) | Out-Null; Start-Sleep -Milliseconds $PrelaunchMs }
                                        Mouse 0x201 $ix 85; Mouse 0x202 $ix 85 }
                  Sheet $f 'bounce' }
        default { throw "unknown scenario $Scenario" }
    }
} finally {
    [Sh]::PostMessage($dock, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 1
    if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $p.Id; 'halo did not close on WM_CLOSE; killed' } else { 'halo closed' }
}
Get-Content (Join-Path $out "$Tag.log") | Select-Object -Last 6
