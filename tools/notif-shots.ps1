# Screenshots the notification stack under the Control Center without touching the live session.
# Launches an exe with HALO_TEST=1 (this also makes the stack DRY-RUN: removals and launches are logged,
# never performed), HALO_TEST_NOHOOK=1, opens the Control Center via WM_APP+20 posted to ITS OWN dock window.
#   tools/notif-shots.ps1 -Exe build/halo-notif.exe -Out build/ns -Steps hover
param([string]$Exe, [string]$Out, [string]$Steps = 'open')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class T {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
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
[T]::SetProcessDPIAware() | Out-Null
$env:HALO_TEST = '1'; $env:HALO_TEST_NOHOOK = '1'; $env:HALO_TEST_CC_DRYRUN = '1'
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
try {
  [T]::PostMessage($dock, 0x8000 + 20, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 400; Shot 'early'
  Start-Sleep -Milliseconds 1400; Shot 'open'
  $ns = [T]::Find($p.Id, 'HaloNotifications'); "ns=$ns"
  if ($Steps -match 'hover') {
    # client px (scale 1.25 here): x,y relative to the stack window
    $l = [IntPtr]((60 -shl 16) -bor 200); [T]::PostMessage($ns, 0x200, [IntPtr]0, $l) | Out-Null; Start-Sleep -Milliseconds 400; Shot 'hover'
    $l = [IntPtr]((25 -shl 16) -bor 391); [T]::PostMessage($ns, 0x200, [IntPtr]0, $l) | Out-Null; Start-Sleep -Milliseconds 400; Shot 'hoverx'
    [T]::PostMessage($ns, 0x201, [IntPtr]1, $l) | Out-Null; Start-Sleep -Milliseconds 600; Shot 'click'
  }
  [T]::PostMessage($dock, 0x8000 + 20, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 1000; Shot 'closed'
} finally { if ($p.HasExited) { "EXITED code=$($p.ExitCode)" } else { Stop-Process -Id $p.Id -Force } }
Get-Content "$Out.log" -ErrorAction SilentlyContinue | Select-Object -Last 25
