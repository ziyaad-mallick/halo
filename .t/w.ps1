param([string]$mode = 'tap')
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern int GetWindowRgn(IntPtr h, IntPtr r);
  [DllImport("gdi32.dll")] public static extern IntPtr CreateRectRgn(int a, int b, int c, int d);
  [DllImport("gdi32.dll")] public static extern int GetRgnBox(IntPtr r, out RECT b);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int l, t, r, b; }
  public static string Fg() { var h = GetForegroundWindow(); uint p; GetWindowThreadProcessId(h, out p); var s = new StringBuilder(256); GetWindowText(h, s, 256); return p + " '" + s + "'"; }
  public static string Rgn() { var h = FindWindow("HaloDockWindow", null); var r = CreateRectRgn(0,0,0,0); GetWindowRgn(h, r); RECT b; GetRgnBox(r, out b); return "region " + (b.r-b.l) + "x" + (b.b-b.t); }
}
"@
. "$PSScriptRoot\h.ps1" 'noop' | Out-Null
"fg before: " + [W]::Fg() + " ; dock " + [W]::Rgn()
