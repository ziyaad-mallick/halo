# Measures the genie animations on a window this script starts (charmap.exe), the same way
# for any version of the mod: it sends WM_SYSCOMMAND to the window (so the mod's hook runs
# inside the app, as for a real click) and samples DwmGetCompositionTimingInfo every ~1 ms
# for the next ~1.3 s. Reports, per animation:
#   call ms      how long the SendMessage took = click to the first animated frame being on
#                screen (the mod blocks the click until then), plus the real minimize/restore
#   frames       frames DWM displayed (qpcFrameDisplayed changes) while the animation ran
#   gap p50/max  interval between displayed frames (ideal on 60 Hz: 16.7 ms)
#   >20 ms       number of intervals over 20 ms (a visible hitch)
#   late/drop/miss  deltas of cFramesLate / cFramesDropped / cFramesMissed
#   cpu          CPU ms of the app (mod worker/capture), halo.exe during the sample (dwm.exe is not readable)
# then Halo's CPU over 3 s of idle afterwards. Refuses to run on the lock screen.
#   tools/genie-bench.ps1 [-Tag old] [-Reps 5] [-HaloPid <pid>]   (default: the live halo.exe)
param([string]$Tag = 'bench', [int]$Reps = 5, [int]$HaloPid = 0, [int]$ObserveMs = 1300)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
New-Item -ItemType Directory -Force $out | Out-Null
Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Diagnostics; using System.Text;
using System.Runtime.InteropServices; using System.Threading;
public static class Gb {
  [StructLayout(LayoutKind.Sequential)] public struct Ratio { public uint n, d; }
  [StructLayout(LayoutKind.Sequential)] public struct Timing {
    public uint cbSize; public Ratio rateRefresh; public ulong qpcRefreshPeriod; public Ratio rateCompose;
    public ulong qpcVBlank, cRefresh; public uint cDXRefresh; public ulong qpcCompose, cFrame; public uint cDXPresent;
    public ulong cRefreshFrame, cFrameSubmitted; public uint cDXPresentSubmitted; public ulong cFrameConfirmed;
    public uint cDXPresentConfirmed; public ulong cRefreshConfirmed; public uint cDXRefreshConfirmed;
    public ulong cFramesLate; public uint cFramesOutstanding; public ulong cFrameDisplayed, qpcFrameDisplayed,
      cRefreshFrameDisplayed, cFrameComplete, qpcFrameComplete, cFramePending, qpcFramePending, cFramesDisplayed,
      cFramesComplete, cFramesPending, cFramesAvailable, cFramesDropped, cFramesMissed, cRefreshNextDisplayed,
      cRefreshNextPresented, cRefreshesDisplayed, cRefreshesPresented, cRefreshStarted, cPixelsReceived,
      cPixelsDrawn, cBuffersEmpty;
  }
  [DllImport("dwmapi.dll")] static extern int DwmGetCompositionTimingInfo(IntPtr h, ref Timing t);
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
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceCounter(out long c);
  [DllImport("kernel32.dll")] static extern bool QueryPerformanceFrequency(out long f);
  public static IntPtr MainWindow(uint pid) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p);
      var sb = new StringBuilder(128); GetWindowText(h, sb, 128);
      if (p == pid && IsWindowVisible(h) && sb.Length > 0) { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
  static double ms(long ticks, long f) { return ticks * 1000.0 / f; }
  // Sends WM_SYSCOMMAND(sc) to h and samples DWM for observeMs. Returns one result line.
  public static string Measure(IntPtr h, int sc, int observeMs, Process[] watch) {
    long f; QueryPerformanceFrequency(out f);
    var cpu0 = new double[watch.Length];
    for (int i = 0; i < watch.Length; i++) { watch[i].Refresh(); cpu0[i] = watch[i].TotalProcessorTime.TotalMilliseconds; }
    var t0 = new Timing(); t0.cbSize = (uint)Marshal.SizeOf(typeof(Timing)); DwmGetCompositionTimingInfo(IntPtr.Zero, ref t0);
    var stop = false; var shown = new List<long>();
    var th = new Thread(() => {
      var t = new Timing(); t.cbSize = (uint)Marshal.SizeOf(typeof(Timing)); ulong last = 0;
      while (!stop) { if (DwmGetCompositionTimingInfo(IntPtr.Zero, ref t) == 0 && t.qpcFrameDisplayed != last) { last = t.qpcFrameDisplayed; lock (shown) shown.Add((long)last); } Thread.Sleep(1); }
    });
    th.Priority = ThreadPriority.AboveNormal; th.Start();
    long a, b; QueryPerformanceCounter(out a);
    IntPtr r; SendMessageTimeout(h, 0x0112, (IntPtr)sc, IntPtr.Zero, 2, 3000, out r);
    QueryPerformanceCounter(out b);
    double callMs = ms(b - a, f);
    Thread.Sleep(Math.Max(0, observeMs - (int)callMs));
    stop = true; th.Join();
    var t1 = new Timing(); t1.cbSize = (uint)Marshal.SizeOf(typeof(Timing)); DwmGetCompositionTimingInfo(IntPtr.Zero, ref t1);
    // Gaps between displayed frames; the animation is over at the first gap past 100 ms (nothing changes).
    var gaps = new List<double>();
    for (int i = 1; i < shown.Count; i++) gaps.Add(ms(shown[i] - shown[i - 1], f));
    int n = 0; while (n < gaps.Count && gaps[n] < 100) n++;
    var run = gaps.GetRange(0, n); run.Sort();
    int over20 = 0; foreach (var g in run) if (g > 20.0) over20++;
    double p50 = run.Count > 0 ? run[run.Count / 2] : 0, max = run.Count > 0 ? run[run.Count - 1] : 0;
    var sb = new StringBuilder();
    sb.AppendFormat("call {0,6:F1} ms | frames {1,3} | gap p50 {2,5:F1} max {3,6:F1} ms | >20ms {4,2} | late {5} drop {6} miss {7} | cpu", callMs, run.Count + 1, p50, max, over20,
      t1.cFramesLate - t0.cFramesLate, t1.cFramesDropped - t0.cFramesDropped, t1.cFramesMissed - t0.cFramesMissed);
    for (int i = 0; i < watch.Length; i++) { watch[i].Refresh(); sb.AppendFormat(" {0} {1:F0}ms", watch[i].ProcessName, watch[i].TotalProcessorTime.TotalMilliseconds - cpu0[i]); }
    return sb.ToString();
  }
}
'@
[Gb]::SetProcessDPIAware() | Out-Null
[Gb]::timeBeginPeriod(1) | Out-Null
$fg = New-Object System.Text.StringBuilder 200
[Gb]::GetWindowText([Gb]::GetForegroundWindow(), $fg, 200) | Out-Null
if ($fg.ToString() -match 'Lock Screen') { 'SKIPPED: the PC is locked'; return }

$halo = if ($HaloPid) { Get-Process -Id $HaloPid } else { Get-Process halo -ErrorAction Stop | Select-Object -First 1 }
$app = Start-Process charmap.exe -PassThru
try {
    $h = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $app.Refresh(); $h = [Gb]::MainWindow([uint32]$app.Id) }
    if ($h -eq [IntPtr]::Zero) { throw 'charmap window not found' }
    Start-Sleep -Seconds 3   # let Halo's tracker list it
    $watch = [System.Diagnostics.Process[]]@($app, $halo)
    $lines = New-Object System.Collections.ArrayList
    function Step($name, $sc) {
        $line = [Gb]::Measure($h, $sc, $ObserveMs, $watch)
        $state = "iconic=$([Gb]::IsIconic($h)) zoomed=$([Gb]::IsZoomed($h))"
        [void]$lines.Add(("{0,-10} {1}  [{2}]" -f $name, $line, $state)); Write-Host $lines[-1]
        Start-Sleep -Milliseconds 400
    }
    for ($r = 1; $r -le $Reps; $r++) {
        Step 'minimize' 0xF020; Step 'restore' 0xF120
        Step 'maximize' 0xF030; Step 'unmaximize' 0xF120
    }
    $halo.Refresh(); $c0 = $halo.TotalProcessorTime.TotalMilliseconds; Start-Sleep -Seconds 3; $halo.Refresh()
    $idle = "halo idle CPU over 3 s after the animations: {0:F0} ms (private {1:F1} MB)" -f ($halo.TotalProcessorTime.TotalMilliseconds - $c0), ($halo.PrivateMemorySize64 / 1MB)
    Write-Host $idle; [void]$lines.Add($idle)
    $lines | Set-Content (Join-Path $out "$Tag-bench.txt")
} finally {
    if (-not $app.HasExited) { $app.CloseMainWindow() | Out-Null; Start-Sleep -Milliseconds 500; if (-not $app.HasExited) { Stop-Process -Id $app.Id } }
}
