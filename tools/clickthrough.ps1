# Checks that clicks outside the dock's input region reach the window underneath.
# Puts a plain (non-topmost) window along the bottom of the screen under a test-mode
# halo, then clicks twice with the REAL mouse: left of the dock (must reach the window)
# and in the dock's empty headroom above the pill (must not). Takes about 3 s and puts
# the cursor back at (960,500). Refuses to run on the lock screen.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Ct {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, IntPtr e);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  public static IntPtr ByPid(uint t) { IntPtr f = IntPtr.Zero; EnumWindows((h,l)=>{ uint pid; GetWindowThreadProcessId(h,out pid); if(pid==t){f=h;return false;} return true;}, IntPtr.Zero); return f; }
}
'@
[Ct]::SetProcessDPIAware() | Out-Null
$sb = New-Object System.Text.StringBuilder 200
[Ct]::GetWindowText([Ct]::GetForegroundWindow(), $sb, 200) | Out-Null
if ($sb.ToString() -match 'Lock Screen') { "SKIPPED: the PC is locked ($sb)"; return }

$env:HALO_TEST = '1'
$p = Start-Process (Join-Path $out 'halo.exe') -PassThru -RedirectStandardError (Join-Path $out 'ct.log')
Remove-Item Env:HALO_TEST
Start-Sleep 3
$dock = [Ct]::ByPid([uint32]$p.Id)
$hits = New-Object System.Collections.ArrayList
$f = New-Object System.Windows.Forms.Form
$f.FormBorderStyle = 'None'; $f.StartPosition = 'Manual'; $f.ShowInTaskbar = $false
$f.Bounds = New-Object System.Drawing.Rectangle 0, 1090, 1920, 110
$f.BackColor = [System.Drawing.Color]::Red
$f.Add_MouseDown({ param($s, $e) [void]$hits.Add("form got click at screen x=$($e.X) y=$($e.Y + 1090)") })
try {
    $f.Show(); $f.Activate(); Start-Sleep -Milliseconds 500; [System.Windows.Forms.Application]::DoEvents()
    function Click($x, $y) {
        [Ct]::SetCursorPos($x, $y) | Out-Null; Start-Sleep -Milliseconds 150
        [Ct]::mouse_event(2, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 50
        [Ct]::mouse_event(4, 0, 0, 0, [IntPtr]::Zero); Start-Sleep -Milliseconds 200
        [System.Windows.Forms.Application]::DoEvents()
    }
    $hits.Clear()
    Click 150 1150;  "left of the dock (150,1150): " + $(if ($hits.Count) { "REACHED the window underneath: $($hits[0])" } else { 'swallowed' })
    $hits.Clear()
    Click 960 1105;  "dock headroom (960,1105):   " + $(if ($hits.Count) { "REACHED the window underneath: $($hits[0])" } else { 'swallowed by the dock window' })
} finally {
    [Ct]::SetCursorPos(960, 500) | Out-Null
    $f.Close()
    [Ct]::PostMessage($dock, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 1
    if (Get-Process -Id $p.Id -ErrorAction SilentlyContinue) { Stop-Process -Id $p.Id }
}
