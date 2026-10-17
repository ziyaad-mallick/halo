# Win+A leak test. Runs build/halo-cc.exe with HALO_TEST_WINKEY=1 (injected keys count), injects a real
# Win+A with SendInput, and screenshots the whole screen so a leaked Windows Quick Settings flyout shows.
# STOP THE LIVE DOCK FIRST (build\guardian.exe --quit) and restart it afterwards. Dry-run only.
param([string]$Exe = '', [string]$Tag = 'wa', [int]$Mode = 0)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
if (!$Exe) { $Exe = Join-Path $repo 'build\halo-cc.exe' }
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class K {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [StructLayout(LayoutKind.Sequential)] public struct KI { public ushort vk, scan; public uint flags, time; public IntPtr extra; public uint pad1, pad2; }
  [StructLayout(LayoutKind.Sequential)] public struct IN { public uint type; public KI ki; }
  [DllImport("user32.dll")] public static extern uint SendInput(uint n, IN[] i, int size);
  public static void Key(ushort vk, bool up) { var i = new IN[1]; i[0].type = 1; i[0].ki.vk = vk; i[0].ki.flags = up ? 2u : 0u; SendInput(1, i, Marshal.SizeOf(typeof(IN))); }
}
'@
[K]::SetProcessDPIAware() | Out-Null
$env:HALO_TEST = '1'; $env:HALO_TEST_WINKEY = '1'; $env:HALO_TEST_CC_DRYRUN = '1'
$p = Start-Process $Exe -PassThru -RedirectStandardError (Join-Path $repo "build\$Tag.log") -WindowStyle Hidden
Start-Sleep -Seconds 2
function Shot($n) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen(0, 0, 0, 0, $bmp.Size)
  $bmp.Save((Join-Path $repo "build\$Tag-$n.png")); $g.Dispose(); $bmp.Dispose()
}
try {
  Shot 'before'
  [K]::Key(0x5B, $false); Start-Sleep -Milliseconds 80      # LWin down
  [K]::Key(0x41, $false); Start-Sleep -Milliseconds 80      # A down
  [K]::Key(0x41, $true);  Start-Sleep -Milliseconds 80      # A up
  [K]::Key(0x5B, $true)                                     # LWin up
  Start-Sleep -Milliseconds 1500
  Shot 'after'
} finally {
  Add-Type -TypeDefinition 'using System;using System.Runtime.InteropServices;public static class W{[DllImport("user32.dll")]public static extern IntPtr FindWindow(string c,string n);[DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);}' -ErrorAction SilentlyContinue
  [W]::PostMessage([W]::FindWindow('HaloDockWindow', $null), 0x10, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Seconds 1
}
Get-Content (Join-Path $repo "build\$Tag.log") | Select-String 'cc\]'
