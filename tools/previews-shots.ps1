# Drives a HALO_TEST instance (build/halo-pv.exe) with posted messages and screenshots the bottom of the
# screen. Usage: powershell -File tools/previews-shots.ps1 -Step <name> [-X 971] [-Y 82] [-Out file.png]
#   Steps: hover (WM_APP+2 at X), leave (WM_APP+3), click (down+up at X,Y in dock-client px),
#          shot (just a screenshot), hidelive / showlive (the owner's dock window, SW_HIDE / SW_SHOWNA),
#          find (print the preview window's handle/rect)
param([string]$Step = 'shot', [int]$X = 0, [int]$Y = 82, [string]$Out = '', [int]$Item = -1, [int]$Frames = 10, [int]$Gap = 0)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices; using System.Text; using System.Collections.Generic;
public class W {
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP f, IntPtr l);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public static List<long[]> Find(string cls){ var r=new List<long[]>(); EnumWindows((h,l)=>{ var sb=new StringBuilder(256); GetClassName(h,sb,256); if(sb.ToString()==cls){uint p; GetWindowThreadProcessId(h,out p); r.Add(new long[]{h.ToInt64(),p});} return true;}, IntPtr.Zero); return r; }
}
'@
[W]::SetProcessDPIAware() | Out-Null
$testPid = (Get-Process halo-pv -ErrorAction SilentlyContinue | Select-Object -First 1).Id
$mine = [IntPtr]0; $live = [IntPtr]0
foreach ($d in [W]::Find('HaloDockWindow')) { if ($d[1] -eq $testPid) { $mine = [IntPtr]$d[0] } else { $live = [IntPtr]$d[0] } }
function Shot($file) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, 520
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen(0, $b.Height - 520, 0, 0, $bmp.Size); $bmp.Save($file); $g.Dispose(); $bmp.Dispose()
}
switch ($Step) {
  'hidelive' { [W]::ShowWindow($live, 0) | Out-Null }
  'showlive' { [W]::ShowWindow($live, 8) | Out-Null }
  'hover' { [W]::PostMessage($mine, 0x8002, [IntPtr]$X, [IntPtr]0) | Out-Null }
  'leave' { [W]::PostMessage($mine, 0x8003, [IntPtr]0, [IntPtr]0) | Out-Null }
  'click' { $l = [IntPtr](($Y -shl 16) -bor $X)
            [W]::PostMessage($mine, 0x201, [IntPtr]1, $l) | Out-Null; Start-Sleep -Milliseconds 80
            [W]::PostMessage($mine, 0x202, [IntPtr]0, $l) | Out-Null }
  'burst' { # click at X,Y, then 10 frames of the dock strip around X as fast as the screen grab allows; frames in $Out_0..9.png
            $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
            $l = [IntPtr](($Y -shl 16) -bor $X)
            if ($Item -ge 0) { [W]::PostMessage($mine, 0x801E, [IntPtr]$Item, [IntPtr]0) | Out-Null } else { [W]::PostMessage($mine, 0x201, [IntPtr]1, $l) | Out-Null; [W]::PostMessage($mine, 0x202, [IntPtr]0, $l) | Out-Null }
            $sw = [Diagnostics.Stopwatch]::StartNew(); $times = @()
            for ($i = 0; $i -lt $Frames; $i++) {
              $bmp = New-Object System.Drawing.Bitmap 360, 160
              $g = [System.Drawing.Graphics]::FromImage($bmp)
              $g.CopyFromScreen($X - 180, $b.Height - 160, 0, 0, $bmp.Size); $bmp.Save("$($Out)_$i.png"); $g.Dispose(); $bmp.Dispose()
              $times += $sw.ElapsedMilliseconds; if ($Gap) { Start-Sleep -Milliseconds $Gap } }
            "frame times ms: $($times -join ',')"; $Out = '' }
  'info' { "mine=$mine live=$live pid=$testPid" }
  'find' { [W]::Find('HaloPreviewsWindow') | ForEach-Object { "$($_[0]) pid $($_[1])" } }
}
if ($Out) { Shot $Out }

