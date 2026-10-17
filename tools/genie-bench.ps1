# Measures the genie animations on a window this script starts (charmap.exe), the same way for
# any version of the mod: it sends WM_SYSCOMMAND to the window (so the mod's hook runs inside the
# app, as for a real click) while build\framecap.exe records, with desktop duplication, when DWM
# really presented each changed desktop frame. Per animation it reports:
#   click->1st   click to the first presented frame that changed after it
#   call ms      how long the SendMessage blocked (the mod holds the click until its first frame is up)
#   frames/exp   frames presented during the animation / frames a perfect run would have (dur x refresh)
#   gap p50/p95/max   interval between presented frames (60 Hz ideal 16.7 ms)
#   late         intervals over 1.5 x the refresh period (a visible hitch), merged = frames DWM merged
#   cpu          CPU ms of the app (capture, mod) and of halo.exe while it ran
# then Halo's CPU over 3 s of idle afterwards. Refuses to run on the lock screen.
#   tools/genie-bench.ps1 [-Tag old] [-Reps 5] [-HaloPid <pid>] [-Shots minimize]   (default: the live halo.exe)
# -Shots <minimize|restore|maximize|unmaximize> also keeps every frame of the last such animation as
# PNGs in build\<Tag>-<kind>\ (cropped to -Crop x,y,w,h, default the whole screen).
param([string]$Tag = 'bench', [int]$Reps = 5, [int]$HaloPid = 0, [int]$ObserveMs = 1500, [string]$Shots = '', [string]$Crop = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
New-Item -ItemType Directory -Force $out | Out-Null
$framecap = Join-Path $out 'framecap.exe'
if (-not (Test-Path $framecap)) { throw 'build tools/framecap first: bash tools/build.sh framecap' }
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Gb {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern IntPtr SendMessageTimeout(IntPtr h, uint m, IntPtr w, IntPtr l, uint f, uint t, out IntPtr r);
  [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsZoomed(IntPtr h);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("winmm.dll")] public static extern uint timeBeginPeriod(uint ms);
  public static IntPtr MainWindow(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      var sb = new StringBuilder(128); GetWindowText(h, sb, 128);
      if (p == pid && IsWindowVisible(h) && sb.Length > 0) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  // Sends WM_SYSCOMMAND(sc); returns the QPC ms before and after the call.
  public static double[] Click(IntPtr h, int sc) {
    double f = System.Diagnostics.Stopwatch.Frequency / 1000.0;
    long a = System.Diagnostics.Stopwatch.GetTimestamp(); IntPtr r;
    SendMessageTimeout(h, 0x0112, (IntPtr)sc, IntPtr.Zero, 2, 3000, out r);
    long b = System.Diagnostics.Stopwatch.GetTimestamp();
    return new double[] { a / f, b / f };
  }
}
'@
[Gb]::SetProcessDPIAware() | Out-Null
[Gb]::timeBeginPeriod(1) | Out-Null
$fg = New-Object System.Text.StringBuilder 200
[Gb]::GetWindowText([Gb]::GetForegroundWindow(), $fg, 200) | Out-Null
if ($fg.ToString() -match 'Lock Screen') { 'SKIPPED: the PC is locked'; return }

function Pct($sorted, $p) { if ($sorted.Count -eq 0) { return 0 }; $sorted[[Math]::Min($sorted.Count - 1, [int][Math]::Floor($p * $sorted.Count))] }

$halo = if ($HaloPid) { Get-Process -Id $HaloPid } else { Get-Process halo -ErrorAction Stop | Select-Object -First 1 }
$app = Start-Process charmap.exe -PassThru
try {
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $app.Refresh(); $h = [Gb]::MainWindow([uint32]$app.Id) }
    if ($h -eq [IntPtr]::Zero) { throw 'charmap window not found' }
    Start-Sleep -Seconds 3   # let Halo's tracker list it
    $lines = New-Object System.Collections.ArrayList
    $periodMs = 16.67
    function Step($name, $sc, $durMs, $shot) {
        $csv = Join-Path $out "$Tag-$name.csv"
        $args = @($ObserveMs, $csv)
        if ($shot) {
            $dir = Join-Path $out "$Tag-$name"; Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory $dir | Out-Null
            $c = if ($Crop) { $Crop -split ',' } else { 0, 0, 0, 0 }
            $args += @($dir, $c[0], $c[1], $c[2], $c[3], 1)
        }
        $cap = Start-Process $framecap -ArgumentList $args -PassThru -WindowStyle Hidden
        Start-Sleep -Milliseconds 500   # duplication starts
        $app.Refresh(); $halo.Refresh()
        $cpuApp0 = $app.TotalProcessorTime.TotalMilliseconds; $cpuHalo0 = $halo.TotalProcessorTime.TotalMilliseconds
        $t = [Gb]::Click($h, $sc)
        Start-Sleep -Milliseconds 900
        $app.Refresh(); $halo.Refresh()
        $cpuApp = $app.TotalProcessorTime.TotalMilliseconds - $cpuApp0; $cpuHalo = $halo.TotalProcessorTime.TotalMilliseconds - $cpuHalo0
        $cap.WaitForExit()
        $rows = Get-Content $csv | Select-Object -Skip 2 | ForEach-Object { $p = $_ -split ','; [pscustomobject]@{ present = [double]$p[1]; acc = [int]$p[2] } }
        $after = @($rows | Where-Object { $_.present -ge $t[0] })
        $gaps = New-Object System.Collections.ArrayList; $merged = 0; $n = 0
        if ($after.Count -gt 0) {
            $n = 1; $merged = if ($after[0].acc -gt 1) { 1 } else { 0 }
            for ($i = 1; $i -lt $after.Count; $i++) {
                $g = $after[$i].present - $after[$i - 1].present
                if ($g -gt 120) { break }
                [void]$gaps.Add($g); $n++; if ($after[$i].acc -gt 1) { $merged++ }
            }
        }
        $s = @($gaps | Sort-Object)
        $late = @($s | Where-Object { $_ -gt 1.5 * $periodMs }).Count
        $first = if ($after.Count) { $after[0].present - $t[0] } else { -1 }
        $exp = [int][Math]::Round($durMs / $periodMs)
        $line = "{0,-10} click->1st {1,6:F1} ms | call {2,6:F1} ms | frames {3,3}/{4,3} | gap p50 {5,5:F1} p95 {6,5:F1} max {7,6:F1} ms | late {8,2} merged {9,2} | cpu app {10,4:F0} halo {11,4:F0} ms  [iconic={12} zoomed={13}]" -f `
            $name, $first, ($t[1] - $t[0]), $n, $exp, (Pct $s 0.5), (Pct $s 0.95), (Pct $s 0.999), $late, $merged, $cpuApp, $cpuHalo, [Gb]::IsIconic($h), [Gb]::IsZoomed($h)
        [void]$lines.Add($line); Write-Host $line
        Start-Sleep -Milliseconds 400
    }
    # one warm-up round (first use of each path), then the measured ones
    $dur = @{ minimize = 500; restore = 500; maximize = 300; unmaximize = 300; 'minimize-max' = 500; 'restore-max' = 500 }
    foreach ($w in 1..($Reps + 1)) {
        $isLast = ($w -eq $Reps + 1)
        foreach ($k in @(@('minimize', 0xF020), @('restore', 0xF120), @('maximize', 0xF030), @('minimize-max', 0xF020), @('restore-max', 0xF120), @('unmaximize', 0xF120))) {
            Step $k[0] $k[1] $dur[$k[0]] ($isLast -and $Shots -eq $k[0])
        }
        if ($w -eq 1) { [void]$lines.Add('(round 1 above was warm-up)'); Write-Host '--- warm-up done' }
    }
    $halo.Refresh(); $c0 = $halo.TotalProcessorTime.TotalMilliseconds; Start-Sleep -Seconds 10; $halo.Refresh()
    $idle = "halo idle CPU over 10 s after the animations: {0:F0} ms (private {1:F1} MB)" -f ($halo.TotalProcessorTime.TotalMilliseconds - $c0), ($halo.PrivateMemorySize64 / 1MB)
    Write-Host $idle; [void]$lines.Add($idle)
    $lines | Set-Content (Join-Path $out "$Tag-bench.txt")
} finally {
    if (-not $app.HasExited) { $app.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 500; if (-not $app.HasExited) { Stop-Process -Id $app.Id } }
}
