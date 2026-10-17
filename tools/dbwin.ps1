# Captures OutputDebugString output (Windhawk's Wh_Log lines from the mod, Halo's "[halo]" lines)
# for -Seconds and writes the lines that match -Match to -Out. Run it beside tools/genie-bench.ps1.
param([int]$Seconds = 60, [string]$Out = 'dbwin.log', [string]$Match = 'WH|halo|genie|handoff|capture|zoom')
Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices; using System.Text;
public static class Dbw {
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateEvent(IntPtr a, bool m, bool i, string n);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateFileMapping(IntPtr f, IntPtr a, uint p, uint hi, uint lo, string n);
  [DllImport("kernel32.dll")] static extern IntPtr MapViewOfFile(IntPtr m, uint acc, uint hi, uint lo, UIntPtr n);
  [DllImport("kernel32.dll")] static extern bool SetEvent(IntPtr h);
  [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr h, uint ms);
  public static List<string> Run(int seconds) {
    var lines = new List<string>();
    IntPtr ready = CreateEvent(IntPtr.Zero, false, false, "DBWIN_BUFFER_READY"), data = CreateEvent(IntPtr.Zero, false, false, "DBWIN_DATA_READY");
    IntPtr map = CreateFileMapping(new IntPtr(-1), IntPtr.Zero, 4, 0, 4096, "DBWIN_BUFFER");
    IntPtr view = MapViewOfFile(map, 4, 0, 0, new UIntPtr(4096));
    var end = DateTime.UtcNow.AddSeconds(seconds);
    SetEvent(ready);
    while (DateTime.UtcNow < end) {
      if (WaitForSingleObject(data, 200) == 0) {
        int pid = Marshal.ReadInt32(view);
        string s = Marshal.PtrToStringAnsi(new IntPtr(view.ToInt64() + 4));
        lines.Add(DateTime.Now.ToString("HH:mm:ss.fff") + " [" + pid + "] " + s.TrimEnd());
        SetEvent(ready);
      }
    }
    return lines;
  }
}
'@
$lines = [Dbw]::Run($Seconds) | Where-Object { $_ -match $Match }
$lines | Set-Content $Out
$lines.Count
