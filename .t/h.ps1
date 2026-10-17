param([string]$cmd, [string]$arg)
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class H {
  public delegate bool EP(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EP p, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint fl, UIntPtr ex);
  [DllImport("user32.dll")] public static extern short GetAsyncKeyState(int vk);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, string t);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  public static string Find(string cls) {
    var sb = new StringBuilder();
    EnumWindows((h, l) => { var c = new StringBuilder(128); GetClassName(h, c, 128); if (c.ToString() == cls) { uint p; GetWindowThreadProcessId(h, out p); sb.Append(h.ToInt64() + ":" + p + " "); } return true; }, IntPtr.Zero);
    return sb.ToString();
  }
  public static void CloseCls(string cls, uint pid) {
    EnumWindows((h, l) => { var c = new StringBuilder(128); GetClassName(h, c, 128); uint p; GetWindowThreadProcessId(h, out p); if (c.ToString() == cls && p == pid) PostMessage(h, 0x10, IntPtr.Zero, IntPtr.Zero); return true; }, IntPtr.Zero);
  }
  public static void Key(byte vk, bool up) { keybd_event(vk, 0, up ? 2u : 0u, UIntPtr.Zero); }
  public static void Chord(byte[] vks) { foreach (var k in vks) { Key(k, false); System.Threading.Thread.Sleep(30); } for (int i = vks.Length - 1; i >= 0; i--) { Key(vks[i], true); System.Threading.Thread.Sleep(30); } }
}
"@
switch ($cmd) {
  'close' { [H]::CloseCls('HaloDockWindow', [uint32]$arg) }
  'find' { [H]::Find($arg) }
  'toggle' { [H]::Chord(@(0x5B, 0x12, 0x54)) }   # Win+Alt+T
  'vis' { [H]::IsWindowVisible([H]::FindWindow('Shell_TrayWnd', $null)) }
  'wintap' { [H]::Chord(@(0x5B)) }
  'wine' { [H]::Chord(@(0x5B, 0x45)) }
  'mods' { foreach ($v in 0x5B,0x5C,0x12,0xA4,0xA5,0x11,0x10) { "{0:X2}={1}" -f $v, (([H]::GetAsyncKeyState($v) -band 0x8000) -ne 0) } }
}
