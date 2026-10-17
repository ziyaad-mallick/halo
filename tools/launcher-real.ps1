# The real flow: an injected Win tap opens the launcher panel AND the real Command Palette, then it is photographed.
# Stops the live dock (guardian --quit), runs this worktree's build with HALO_TEST_WINKEY=1 (injected Win counts)
# and power actions as dry runs, taps Win, takes full-screen shots, taps Win again to close both, then ALWAYS
# starts the live guardian again. Never run it while the owner is using the machine.
#   tools/launcher-real.ps1 -Live C:\path\to\main\build [-Tag real] [-Exe build\halo-launcher.exe]
param([Parameter(Mandatory)][string]$Live, [string]$Tag = 'real', [string]$Exe = '', [string]$Recent = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
if (-not $Exe) { $Exe = Join-Path $out 'halo-launcher.exe' }
if (Get-Process -Name 'halo-cc*' -ErrorAction SilentlyContinue) { throw 'a halo-cc process is running; not touching the live dock' }
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Lr {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
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
[Lr]::SetProcessDPIAware() | Out-Null
$W = [Lr]::GetSystemMetrics(0); $H = [Lr]::GetSystemMetrics(1)
function Shot($name) {
    $b = New-Object System.Drawing.Bitmap $W, $H
    $g = [System.Drawing.Graphics]::FromImage($b); $g.CopyFromScreen(0, 0, 0, 0, $b.Size); $g.Dispose()
    $path = Join-Path $out "$Tag-$name.png"; $b.Save($path); $b.Dispose(); "shot $path"
}
function TapWin { [Lr]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40; [Lr]::keybd_event(0x5B, 0, 2, [UIntPtr]::Zero) }

$guardian = Join-Path $Live 'guardian.exe'
$log = Join-Path $out "$Tag.log"
$p = $null
try {
    & $guardian --quit; Start-Sleep -Seconds 2
    $env:HALO_TEST_WINKEY = '1'; $env:HALO_TEST_POWER_DRYRUN = '1'
    if ($Recent) { $env:HALO_TEST_RECENT = $Recent }
    $p = Start-Process -FilePath $Exe -PassThru -RedirectStandardError $log -RedirectStandardOutput (Join-Path $out "$Tag.out")
    Start-Sleep -Seconds 3
    Shot 'before'
    TapWin; Start-Sleep -Milliseconds 1500; Shot 'open'
    Start-Sleep -Milliseconds 800;  Shot 'open2'
    TapWin; Start-Sleep -Milliseconds 1200; Shot 'closed'
} finally {
    if ($p -and -not $p.HasExited) {
        $dock = [Lr]::Find($p.Id, 'HaloDockWindow')
        if ($dock -ne [IntPtr]::Zero) { [Lr]::PostMessage($dock, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
        $p.WaitForExit(3000) | Out-Null
        if (-not $p.HasExited) { $p.Kill() }
    }
    Remove-Item Env:HALO_TEST_WINKEY, Env:HALO_TEST_POWER_DRYRUN -ErrorAction SilentlyContinue
    Start-Process -FilePath $guardian -WindowStyle Hidden
    Start-Sleep -Seconds 3
    'live guardian running again: ' + [bool](Get-Process -Name guardian -ErrorAction SilentlyContinue)
}
Get-Content $log | Select-String 'launcher|palette|stack|glue'
