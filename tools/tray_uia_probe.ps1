<#
.SYNOPSIS
  READ-ONLY spike: can UI Automation enumerate the Windows tray icons while Halo has the taskbar hidden?

.DESCRIPTION
  Finds Shell_TrayWnd, reports its visibility and rect, then walks its UIA subtree (ControlView or
  RawView, depth limited) and prints one line per element. It never clicks, invokes, toggles,
  expands, focuses, moves or shows anything. It only reads properties and lists which patterns
  are supported. The one LegacyIAccessible read is the DefaultAction string (a property).

  Privacy: raw element names are never printed unless -ShowNames is passed. By default each name is
  reduced to a generic CATEGORY (clock, volume, network, chevron, app, ...). Do not commit output
  produced with -ShowNames.

.PARAMETER MaxDepth    Maximum depth below the root to walk (default 8).
.PARAMETER View        Control or Raw (default Control).
.PARAMETER MaxElements Hard cap on elements visited (default 600).
.PARAMETER BudgetSec   Wall-clock budget for the walk (default 8).
.PARAMETER ShowNames   Also print the raw Name. Local debugging only.
.PARAMETER Summary     Print only the summary block, not one line per element.
.PARAMETER ListenSec   Passively listen this many seconds for UIA StructureChanged and Name-changed
                       events under the tray and report counts (default 0 = off). Subscribing
                       changes nothing on screen. 70 is enough to see a clock minute rollover.
#>
[CmdletBinding()]
param(
    [int]$MaxDepth = 8,
    [ValidateSet('Control', 'Raw')][string]$View = 'Control',
    [int]$MaxElements = 600,
    [double]$BudgetSec = 8,
    [switch]$ShowNames,
    [switch]$Summary,
    [int]$ListenSec = 0
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class HaloTrayNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern int GetSystemMetrics(int i);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int n);
    delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr parent, EnumProc p, IntPtr l);
    public static string ClassOf(IntPtr h) { var sb = new StringBuilder(256); GetClassName(h, sb, 256); return sb.ToString(); }
    static string Describe(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        return h.ToInt64().ToString("X") + "|" + ClassOf(h) + "|" + (IsWindowVisible(h) ? "visible" : "hidden") + "|" + r.L + "," + r.T + "," + r.R + "," + r.B;
    }
    // Top-level windows owned by one process: handle|class|visibility|rect.
    public static List<string> TopLevelOf(uint pid) {
        var res = new List<string>();
        EnumWindows((h, l) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid) res.Add(Describe(h));
            return true;
        }, IntPtr.Zero);
        return res;
    }
    public static List<string> ChildrenOf(IntPtr parent) {
        var res = new List<string>();
        EnumChildWindows(parent, (h, l) => { res.Add(Describe(h)); return true; }, IntPtr.Zero);
        return res;
    }
}
'@

function Get-NameCategory([string]$name, [string]$id, [string]$cls) {
    # Reduces a raw name to a generic category. The raw name is never emitted by this function.
    $n = if ($name) { $name } else { '' }
    if ($id -match 'Clock' -or $n -match '(?i)\b\d{1,2}:\d{2}\b|^clock|date and time') { return 'clock' }
    if ($n -match '(?i)show hidden icons|hidden icons|overflow') { return 'chevron' }
    if ($id -match 'Chevron|Overflow' -or $cls -match 'Chevron') { return 'chevron' }
    if ($n -match '(?i)volume|speaker|sound|headphone|audio') { return 'volume' }
    if ($n -match '(?i)network|wi-?fi|ethernet|internet|vpn|connected|no internet') { return 'network' }
    if ($n -match '(?i)battery|power|plugged') { return 'battery' }
    if ($n -match '(?i)keyboard|input indicator|language|^[A-Z]{3}$') { return 'input-indicator' }
    if ($n -match '(?i)notification|action center|quick settings') { return 'notification-center' }
    if ($n -match '(?i)^start$|^search|task view|widgets|copilot|show desktop|^home$') { return 'taskbar-shell' }
    if ($n -eq '') { return 'unnamed' }
    return 'app-or-other'
}

function Format-Rect($r) {
    if ($r.IsEmpty) { return 'empty' }
    return ('{0:F0},{1:F0},{2:F0}x{3:F0}' -f $r.Left, $r.Top, $r.Width, $r.Height)
}

function Get-PatternInfo($el) {
    # Lists supported patterns and the LegacyIAccessible DefaultAction string. Read only.
    $short = @()
    try {
        foreach ($p in $el.GetSupportedPatterns()) {
            $short += ($p.ProgrammaticName -replace 'PatternIdentifiers\.Pattern$', '')
        }
    } catch { $short += 'patterns-unreadable' }
    $legacy = ''
    if ($short -contains 'LegacyIAccessible') {
        try {
            $o = $null
            if ($el.TryGetCurrentPattern([System.Windows.Automation.LegacyIAccessiblePattern]::Pattern, [ref]$o)) {
                $legacy = $o.Current.DefaultAction   # property read only
            }
        } catch { $legacy = '?' }
    }
    [pscustomobject]@{
        Patterns = ($short -join '+'); DefaultAction = $legacy
        Invoke = ($short -contains 'Invoke'); Toggle = ($short -contains 'Toggle')
        ExpandCollapse = ($short -contains 'ExpandCollapse'); Legacy = ($short -contains 'LegacyIAccessible')
    }
}

# Make Win32 rects and UIA rects use the same (physical) pixels. Affects only this process.
[void][HaloTrayNative]::SetProcessDPIAware()
$sw = [System.Diagnostics.Stopwatch]::StartNew()

# Task-list button AutomationIds embed app ids, so only a short allow-list of AutomationIds is printed raw.
$safeIds = 'StartButton', 'SystemTrayIcon', 'TaskbarFrame', 'SearchButton', 'TaskViewButton', 'WidgetsButton'
function Get-SafeId([string]$id) {
    if ($ShowNames -or [string]::IsNullOrEmpty($id) -or ($safeIds -contains $id)) { return $id }
    if ($id -match '^Appid:') { return '<appid>' }
    return '<redacted>'
}

# 1. The window
$tray = [HaloTrayNative]::FindWindow('Shell_TrayWnd', $null)
Write-Output ('# os=' + [Environment]::OSVersion.Version + ' view=' + $View + ' maxDepth=' + $MaxDepth)
if ($tray -eq [IntPtr]::Zero) { Write-Output 'Shell_TrayWnd: NOT FOUND'; exit 2 }
$rc = New-Object HaloTrayNative+RECT
[void][HaloTrayNative]::GetWindowRect($tray, [ref]$rc)
$vis = [HaloTrayNative]::IsWindowVisible($tray)
Write-Output ('primary screen (physical px): {0}x{1}' -f [HaloTrayNative]::GetSystemMetrics(0), [HaloTrayNative]::GetSystemMetrics(1))
Write-Output ('Shell_TrayWnd: found, IsWindowVisible={0}, rect={1},{2},{3},{4}' -f $vis, $rc.L, $rc.T, $rc.R, $rc.B)

# Explorer's top-level windows (class names only), to see overflow windows that exist right now.
$ownerPid = [uint32]0
[void][HaloTrayNative]::GetWindowThreadProcessId($tray, [ref]$ownerPid)
$tops = [HaloTrayNative]::TopLevelOf($ownerPid)
$interesting = $tops | Where-Object { $_ -match 'Shell_TrayWnd|Shell_SecondaryTrayWnd|Overflow|NotifyIcon|TrayNotify' }
Write-Output '# explorer top-level windows of interest (handle|class|visibility|rect):'
foreach ($t in $interesting) { Write-Output ('  ' + $t) }
Write-Output ('# secondary taskbars: ' + @($tops | Where-Object { $_ -match '\|Shell_SecondaryTrayWnd\|' }).Count)
Write-Output ('# overflow-style top-level windows present now: ' + @($tops | Where-Object { $_ -match 'Overflow' }).Count)
Write-Output '# Win32 child windows of Shell_TrayWnd (class|visibility|rect):'
foreach ($c in [HaloTrayNative]::ChildrenOf($tray)) { Write-Output ('  ' + ($c -replace '^[0-9A-F]+\|', '')) }

# 2. The UIA walk
# On Windows 11 the XAML tray is NOT under the Shell_TrayWnd UIA node itself. It is under the child
# HWND of class Windows.UI.Composition.DesktopWindowContentBridge. So walk Shell_TrayWnd and each such bridge.
$roots = @($tray)
foreach ($c in [HaloTrayNative]::ChildrenOf($tray)) {
    $f = $c.Split('|')
    if ($f[1] -eq 'Windows.UI.Composition.DesktopWindowContentBridge') { $roots += [IntPtr][Convert]::ToInt64($f[0], 16) }
}
$walker = if ($View -eq 'Raw') { [System.Windows.Automation.TreeWalker]::RawViewWalker } else { [System.Windows.Automation.TreeWalker]::ControlViewWalker }

$script:rows = New-Object System.Collections.Generic.List[object]
$script:truncated = $false
function Walk($el, [int]$depth) {
    if ($script:rows.Count -ge $MaxElements -or $sw.Elapsed.TotalSeconds -gt $BudgetSec) { $script:truncated = $true; return }
    $c = $el.Current
    $pi = Get-PatternInfo $el
    $cat = Get-NameCategory $c.Name $c.AutomationId $c.ClassName
    $row = [pscustomobject]@{
        Depth = $depth; Type = ($c.ControlType.ProgrammaticName -replace '^ControlType\.', '')
        Class = $c.ClassName; AutoId = (Get-SafeId $c.AutomationId); RawAutoId = $c.AutomationId; Category = $cat
        Offscreen = $c.IsOffscreen; Rect = (Format-Rect $c.BoundingRectangle)
        Patterns = $pi.Patterns; DefaultAction = $pi.DefaultAction
        Invoke = $pi.Invoke; Legacy = $pi.Legacy; Toggle = $pi.Toggle; Expand = $pi.ExpandCollapse
        Name = if ($ShowNames) { $c.Name } else { $null }
    }
    $script:rows.Add($row)
    if ($depth -ge $MaxDepth) { return }
    $ch = $walker.GetFirstChild($el)
    while ($null -ne $ch) {
        Walk $ch ($depth + 1)
        if ($script:truncated) { return }
        $ch = $walker.GetNextSibling($ch)
    }
}
foreach ($rh in $roots) {
    $re = [System.Windows.Automation.AutomationElement]::FromHandle($rh)
    Walk $re 0
}
$rows = $script:rows
$walkMs = $sw.ElapsedMilliseconds

if (-not $Summary) {
    Write-Output '# UIA elements (depth|type|class|autoId|category|offscreen|rect|patterns|legacyDefaultAction)'
    foreach ($r in $rows) {
        $line = '{0}{1}|{2}|{3}|{4}|{5}|off={6}|{7}|{8}|da={9}' -f ('  ' * $r.Depth), $r.Depth, $r.Type, $r.Class, $r.AutoId, $r.Category, $r.Offscreen, $r.Rect, $r.Patterns, $r.DefaultAction
        if ($ShowNames) { $line += '|name=' + $r.Name }
        Write-Output $line
    }
}

# 3. Classification: tray-icon candidates carry a SystemTray/NotifyIcon marker in class or automation id.
$trayish = @($rows | Where-Object { $_.Class -match 'SystemTray|NotifyIcon|TrayIcon' -or $_.RawAutoId -match 'SystemTray|NotifyIcon|TrayIcon|Chevron|Overflow' })
$taskBtns = @($rows | Where-Object { $_.Class -eq 'Taskbar.TaskListButtonAutomationPeer' })
Write-Output ('taskbar task-list buttons (running/pinned apps, not tray): {0}' -f $taskBtns.Count)
Write-Output '# --- summary ---'
Write-Output ('elements visited: {0} (truncated={1}), walk time {2} ms' -f $rows.Count, $script:truncated, $walkMs)
Write-Output ('tray-marked elements (class/autoId): {0}' -f $trayish.Count)
Write-Output '# tray-marked elements by (category, offscreen):'
$trayish | Group-Object Category, Offscreen | ForEach-Object { Write-Output ('  {0} -> {1}' -f $_.Name, $_.Count) }
Write-Output '# tray-marked elements by (type, class, autoId):'
$trayish | Group-Object Type, Class, AutoId | ForEach-Object { Write-Output ('  {0} -> {1}' -f $_.Name, $_.Count) }
Write-Output ('# pattern support on tray-marked elements: Invoke={0} LegacyIAccessible={1} Toggle={2} ExpandCollapse={3}' -f @($trayish | Where-Object Invoke).Count, @($trayish | Where-Object Legacy).Count, @($trayish | Where-Object Toggle).Count, @($trayish | Where-Object Expand).Count)
Write-Output '# LegacyIAccessible DefaultAction values on tray-marked elements:'
$trayish | Where-Object { $_.DefaultAction } | Group-Object DefaultAction | ForEach-Object { Write-Output ('  "{0}" x{1}' -f $_.Name, $_.Count) }
Write-Output ('# elements with non-empty rect: {0}; IsOffscreen=true: {1}' -f @($rows | Where-Object { $_.Rect -ne 'empty' }).Count, @($rows | Where-Object { $_.Offscreen }).Count)
# Registry view (read only, counts only): every app that has registered a notification-area icon.
$nis = 'HKCU:\Control Panel\NotifyIconSettings'
if (Test-Path $nis) {
    $subs = @(Get-ChildItem $nis -ErrorAction SilentlyContinue | ForEach-Object { Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue })
    Write-Output ('# registry NotifyIconSettings: {0} registered icons, IsPromoted=1: {1}, IsPromoted=0: {2}, unset: {3}' -f $subs.Count, @($subs | Where-Object { $_.IsPromoted -eq 1 }).Count, @($subs | Where-Object { $_.IsPromoted -eq 0 }).Count, @($subs | Where-Object { $null -eq $_.IsPromoted }).Count)
} else { Write-Output '# registry NotifyIconSettings: key not present' }

# Optional passive event listening (no UI change).
if ($ListenSec -gt 0) {
    Add-Type -ReferencedAssemblies UIAutomationClient, UIAutomationTypes -TypeDefinition @'
using System; using System.Threading; using System.Windows.Automation;
public class HaloTrayListener {
    public int Structure, NameChanged, Other;
    public string Last = "";
    public void Start(AutomationElement el) {
        Automation.AddStructureChangedEventHandler(el, TreeScope.Subtree, (s, e) => { Interlocked.Increment(ref Structure); Last = "structure:" + e.StructureChangeType; });
        Automation.AddAutomationPropertyChangedEventHandler(el, TreeScope.Subtree, (s, e) => { if (e.Property == AutomationElement.NameProperty) Interlocked.Increment(ref NameChanged); else Interlocked.Increment(ref Other); }, AutomationElement.NameProperty, AutomationElement.BoundingRectangleProperty);
    }
    public void Stop() { Automation.RemoveAllEventHandlers(); }
}
'@
    $lis = New-Object HaloTrayListener
    $lis.Start([System.Windows.Automation.AutomationElement]::FromHandle($roots[-1]))
    Start-Sleep -Seconds $ListenSec
    $lis.Stop()
    Write-Output ('# events over {0}s while hidden: StructureChanged={1} NameChanged={2} OtherPropertyChanged={3} last={4}' -f $ListenSec, $lis.Structure, $lis.NameChanged, $lis.Other, $lis.Last)
}
Write-Output ('done in {0} ms' -f $sw.ElapsedMilliseconds)
exit 0
