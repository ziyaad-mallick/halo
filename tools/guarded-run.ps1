# End-to-end: guardian hides the native taskbar and runs halo; we screenshot
# the dock, close halo normally, and check the taskbar is back as it was.
param([string]$Tag = 'guarded')
$repo = Split-Path $PSScriptRoot
$out = Join-Path $repo 'build'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Gr {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct APPBARDATA { public int cbSize; public IntPtr hWnd; public uint cb; public uint edge; public RECT rc; public IntPtr lParam; }
  [DllImport("shell32.dll")] public static extern UIntPtr SHAppBarMessage(uint m, ref APPBARDATA d);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  public delegate bool EnumProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  public static string Taskbar() {
    var d = new APPBARDATA(); d.cbSize = Marshal.SizeOf(d);
    return "visible=" + IsWindowVisible(FindWindow("Shell_TrayWnd", null)) + " state=" + SHAppBarMessage(4, ref d);
  }
  public static IntPtr DockWindow(uint target) {
    IntPtr found = IntPtr.Zero;
    EnumWindows((h, l) => { uint pid; GetWindowThreadProcessId(h, out pid); var sb = new StringBuilder(64); GetClassName(h, sb, 64);
      if (pid == target && sb.ToString() == "HaloDockWindow") { found = h; return false; } return true; }, IntPtr.Zero);
    return found;
  }
}
'@
[Gr]::SetProcessDPIAware() | Out-Null
$sb = New-Object System.Text.StringBuilder 200
[Gr]::GetWindowText([Gr]::GetForegroundWindow(), $sb, 200) | Out-Null
if ($sb.ToString() -match 'Lock Screen') { "SKIPPED: the PC is locked"; return }

"before:  " + [Gr]::Taskbar()
[Gr]::SetCursorPos(960, 500) | Out-Null
$g = Start-Process (Join-Path $out 'guardian.exe') -PassThru
Start-Sleep 4
"running: " + [Gr]::Taskbar()
$halo = Get-Process halo -ErrorAction SilentlyContinue | Select-Object -First 1
"halo pid: $($halo.Id)"
[Gr]::SetCursorPos(900, 1199) | Out-Null; Start-Sleep -Milliseconds 900
$b = New-Object System.Drawing.Bitmap 1920, 200; $gr = [System.Drawing.Graphics]::FromImage($b)
$gr.CopyFromScreen(0, 1000, 0, 0, $b.Size); $b.Save((Join-Path $out "$Tag-dock.png")); $gr.Dispose(); $b.Dispose()
[Gr]::SetCursorPos(960, 500) | Out-Null; Start-Sleep 1
if ($halo) { [Gr]::PostMessage([Gr]::DockWindow([uint32]$halo.Id), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
Start-Sleep 3
"after:   " + [Gr]::Taskbar() + "  guardian alive=" + [bool](Get-Process -Id $g.Id -ErrorAction SilentlyContinue)
"--- guardian.log (last 6):"; Get-Content "$env:LOCALAPPDATA\Halo\guardian.log" -Tail 6
