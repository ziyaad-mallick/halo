# GPU cost of a TEST Halo instance, from the Windows GPU Engine counters (3D engine, per process).
# Halo only draws surfaces; the compositor work (blur, clips, shadows) lands on dwm.exe, so both are reported.
# Launches -Exe in test mode (HALO_TEST=1: no hook, dry-run), then samples once a second through four phases:
#   idle         nothing happening
#   hover        the pointer swept across the dock (WM_APP+2) so the magnification wave runs constantly
#   notif_open   the notification column opened (WM_APP+21) and left still
#   after_close  the column closed again
# Reports median and peak % per phase for the Halo process and for dwm.exe. dwm.exe also serves everything else on
# screen, so read its idle row as the floor and the others as the increase over it.
#   tools/gpu-measure.ps1 -Exe build/halo.exe [-Seconds 6] [-Theme dark] [-Appdata <dir with Halo\settings.ini>]
param([string]$Exe, [int]$Seconds = 6, [string]$Theme = 'dark', [string]$Appdata = '')
$ErrorActionPreference = 'Stop'
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class G {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public static IntPtr Find(uint pid, string cls) { IntPtr r = IntPtr.Zero;
    EnumWindows((h,l)=>{ uint p; GetWindowThreadProcessId(h,out p); var sb=new StringBuilder(64); GetClassName(h,sb,64);
      if(p==pid && sb.ToString()==cls){ r=h; return false;} return true;}, IntPtr.Zero); return r; }
}
'@
$env:HALO_TEST = '1'; $env:HALO_TEST_NOHOOK = '1'; $env:HALO_TEST_CC_DRYRUN = '1'; $env:HALO_TEST_POWER_DRYRUN = '1'; $env:HALO_THEME = $Theme
if ($Appdata) { $env:LOCALAPPDATA = $Appdata }
$p = Start-Process $Exe -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 4
$dock = [G]::Find($p.Id, 'HaloDockWindow')
$dwm = (Get-Process dwm | Select-Object -First 1).Id
function Sample([int]$secs, [scriptblock]$drive) {
    # The sums are taken inside the job: counter samples do not survive crossing out of it as objects.
    $job = Start-Job {
        param($n, $mine, $dwm)
        Get-Counter '\GPU Engine(*engtype_3D)\Utilization Percentage' -SampleInterval 1 -MaxSamples $n | ForEach-Object {
            $m = 0.0; $d = 0.0
            foreach ($c in $_.CounterSamples) {
                if ($c.InstanceName -match "pid_${mine}_") { $m += $c.CookedValue }
                if ($c.InstanceName -match "pid_${dwm}_") { $d += $c.CookedValue }
            }
            "$m $d"
        }
    } -ArgumentList $secs, $p.Id, $dwm
    if ($drive) { & $drive }
    $rows = @(Receive-Job $job -Wait); Remove-Job $job
    $mine = @($rows | ForEach-Object { [double]($_ -split ' ')[0] }); $dw = @($rows | ForEach-Object { [double]($_ -split ' ')[1] })
    $f = { param($a) $o = @($a | Sort-Object); '{0,5:N1} median {1,5:N1} peak' -f $o[[int][math]::Floor($o.Count / 2)], $o[-1] }
    [pscustomobject]@{ halo = (& $f $mine); dwm = (& $f $dw) }
}
try {
    $r = [ordered]@{}
    $r.idle = Sample $Seconds $null
    $r.hover = Sample $Seconds {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        while ($sw.ElapsedMilliseconds -lt ($Seconds * 1000)) {
            $x = 600 + 720 * (0.5 + 0.5 * [math]::Sin($sw.ElapsedMilliseconds / 450.0))
            [G]::PostMessage($dock, 0x8002, [IntPtr][int]$x, [IntPtr]::Zero) | Out-Null
            Start-Sleep -Milliseconds 16
        }
        [G]::PostMessage($dock, 0x8003, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    }
    [G]::PostMessage($dock, 0x8000 + 21, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 300
    "notification column open: $([G]::Find($p.Id, 'HaloNotifications') -ne [IntPtr]::Zero)"
    $r.notif_open = Sample $Seconds $null
    [G]::PostMessage($dock, 0x8000 + 21, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 1
    $r.after_close = Sample 3 $null
    foreach ($k in $r.Keys) { "{0,-12} halo {1}   dwm {2}" -f $k, $r[$k].halo, $r[$k].dwm }
    $pr = Get-Process -Id $p.Id; "private MB {0:N1}, threads {1}" -f ($pr.PrivateMemorySize64 / 1MB), $pr.Threads.Count
} finally { if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
