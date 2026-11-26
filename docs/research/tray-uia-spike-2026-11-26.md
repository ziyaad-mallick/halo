# Tray icons through UI Automation while the taskbar is hidden (spike, 2026-11-26)

Question: with Halo's guardian hiding Explorer's taskbar (auto-hide, then SW_HIDE), can Halo still
read the notification-area (tray) icons through UI Automation (UIA), without injecting into explorer.exe?

Short answer: yes for the icons that are on the tray strip, no for the overflow icons until the
overflow flyout is open. Activation is possible through the Invoke pattern, but that was not tested
(it is forbidden in a read-only spike).

## Method

Everything here is read-only. The probe `tools/tray_uia_probe.ps1` never clicks, invokes, toggles,
expands, focuses, moves or shows anything. It only reads properties, lists supported patterns
(`GetSupportedPatterns`), and reads the LegacyIAccessible DefaultAction string where that pattern exists.
Raw names are never printed unless `-ShowNames` is given (that output is not committed). Each element
gets a generic category instead of its name, and task-list AutomationIds (which embed app ids) print
as `<appid>`.

Machine: Windows 11 build 10.0.26300, one monitor 1920x1200 physical px, Halo running (taskbar hidden).

Commands run (Windows PowerShell 5.1, from the worktree):

    powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/tray_uia_probe.ps1
    powershell.exe ... tools/tray_uia_probe.ps1 -View Raw -Summary
    powershell.exe ... tools/tray_uia_probe.ps1 -ListenSec 70      # passive event listening
    (plus throwaway scripts, not committed: desktop-wide FindAll by class name, per-HWND FromHandle probes)

## Findings (redacted output)

    primary screen (physical px): 1920x1200
    Shell_TrayWnd: found, IsWindowVisible=False, rect=0,1198,1920,1258
    secondary taskbars: 0
    overflow-style top-level windows present now: 0
    Win32 children of Shell_TrayWnd (all hidden): DesktopWindowContentBridge, InputSite.WindowClass,
      CoreWindow, Start, TrayDummySearchControl, TrayNotifyWnd, ReBarWindow32, MSTaskSwWClass, MSTaskListWClass

1. The window is hidden and parked at y=1198, so only 2 px of a 60 px bar would overlap the display.
2. **UIA on the Shell_TrayWnd node itself returns zero children.** The XAML tree hangs off the child
   HWND of class `Windows.UI.Composition.DesktopWindowContentBridge` (23 descendants in raw
   FindAll). The probe therefore walks Shell_TrayWnd plus that bridge. A desktop-wide search by class
   name (`Shell_TrayWnd`, `SystemTray.NormalButton`, `TrayNotifyWnd`, the bridge, `TopLevelWindowForOverflowXamlIsland`,
   `NotifyIconOverflowWindow`) finds nothing: a hidden window is pruned from the desktop's UIA children, so
   the walk must start from the HWND with `AutomationElement.FromHandle`.
3. Tree under the bridge (Control view, 26 elements, 364 ms; Raw view, 61 elements, 400 to 660 ms):

        Pane  Windows.UI.Input.InputSite.WindowClass
          Pane  Taskbar.TaskbarFrameAutomationPeer  (autoId TaskbarFrame)
            Button  ToggleButton (autoId StartButton)                        category: start
            Button  Taskbar.TaskListButtonAutomationPeer (autoId <appid>) x14   running/pinned apps, not tray
          Button  SystemTray.NormalButton      (SystemTrayIcon)  Invoke+ScrollItem   category: chevron
          Button  SystemTray.AccentButton      (SystemTrayIcon)  Invoke+ScrollItem   category: network
          Button  SystemTray.OmniButtonCenter  (SystemTrayIcon)  Invoke+ScrollItem   category: volume
          Button  SystemTray.AccentButton      (SystemTrayIcon)  Invoke+ScrollItem   category: battery
          Button  SystemTray.OmniButtonLeft    (SystemTrayIcon)  Invoke+ScrollItem   category: clock
          Button  SystemTray.OmniButtonRight   (SystemTrayIcon)  Invoke+ScrollItem   category: notification center
          Button  SystemTray.ShowDesktopButton (SystemTrayIcon)  Invoke+ScrollItem

4. All 7 tray elements have `IsOffscreen=False` and real bounding rectangles even though the window is
   hidden. They sit at x 1618 to 1920 inside the parked bar. None exposes LegacyIAccessible, Toggle or
   ExpandCollapse. So there is no DefaultAction string; Invoke is the only action pattern.
5. At the time of the run the tray strip held **0 app icons**: 1 chevron, 1 network, 1 volume,
   1 battery, 1 clock, 1 notification center, 1 show-desktop. App icons (VPN, chat, game launcher,
   keyboard layout) are not on the strip. Whether they are all behind the chevron is UNVERIFIED (the
   chevron cannot be opened in this spike).
6. No overflow window or element exists while the flyout is closed. Explorer owns `XamlExplorerHostIslandWindow`
   (1, empty), `SystemTray_Main` (hidden, 0 UIA descendants) and 9 `Xaml_WindowedPopupClass` (hidden, 0
   descendants), but no `TopLevelWindowForOverflowXamlIsland` or `NotifyIconOverflowWindow`.
7. Registry side channel (read-only, counts only): `HKCU\Control Panel\NotifyIconSettings` lists 101
   registered tray icons ever seen, 2 with IsPromoted=1 (shown on the strip), 99 unset (overflow by
   default). It does not say which are running now, but it holds each icon's executable path and
   promotion flag, so it can label overflow items once they are readable. Only the counts are recorded here.
8. Events while hidden: a 70 s passive listener on the bridge subtree received StructureChanged=2
   (last: ChildRemoved) and Name-changed=6 (cause not identified; a clock minute rollover is the likely source), 0 other
   property changes. UIA events do fire for a hidden taskbar.
9. The probe exits in about 1.0 to 1.3 s wall time (about 0.4 s of UIA), exit code 0, and re-runs
   cleanly (run twice in a row).

## Verdict

- (a) Listing visible tray icons: **feasible.** Enumerate Button elements with AutomationId
  `SystemTrayIcon` under the bridge. Class names split system buttons (Omni*, Accent, ShowDesktop,
  chevron) from ordinary app icons (`SystemTray.NormalButton`). Note this build had no app icons on the
  strip, so app-icon element shape (name, tooltip, any extra properties) is UNVERIFIED; it is expected
  to be NormalButton with the app name.
- (b) Listing overflow icons: **not feasible passively.** Nothing exists to read while the flyout is
  closed. It is UNVERIFIED whether the flyout window appears hidden-but-populated after one open; the
  only way to read it is to open it (Invoke the chevron), which this spike may not do. The registry
  list is a partial substitute for names, not for live state.
- (c) Activating an icon later: Invoke is advertised on every tray button, so
  `InvokePattern.Invoke()` is the natural call. UNVERIFIED whether Invoke on an app icon behaves like a
  left click (opens the app's flyout) when the taskbar window is hidden, and the XAML tray has no
  LegacyIAccessible DoDefaultAction fallback. Right-click menus have no UIA pattern, so a real mouse
  click, or a context-menu path of our own, would be needed for them. Test Invoke on a harmless
  element (the show-desktop or clock button) in a supervised follow-up before building on it.

## Risks

- **Windows version drift.** Class names (`SystemTray.*`), the bridge HWND, and the fact that the tray
  XAML is not under the Shell_TrayWnd UIA node are all internal to Explorer and changed between
  Windows 11 releases. Match on AutomationId plus control type first, class second, and fail soft.
- **IsOffscreen lies for a hidden window.** It reported False for everything here, so it cannot gate
  anything. Treat rectangles as stale for placement: they describe the parked bar, not where a flyout
  would go.
- **A hidden or restored bar changes the tree.** Auto-hide and un-hide move the rects; the guardian
  restoring the taskbar on exit would repopulate the visible state. Re-check after restore.
- **Performance.** 26 elements (Control view) cost about 0.4 s in PowerShell with a property read per
  pattern. Native code with a UIA CacheRequest (one batched call) should be far cheaper. The walk
  should be scoped to the bridge, depth 4, never the desktop (a desktop-wide FindAll took about 0.35 s per class).
- **Cross-process calls.** UIA reads go to explorer.exe. A hung Explorer can stall them, so use a
  timeout and a worker thread, never the UI thread.
- **Privacy.** Names, tooltips and AppIds can carry account or VPN names. Keep them in memory, never log them.

## Refresh without polling

UIA events worked on the hidden tree (item 8). Subscribe once on the bridge element with
`AddStructureChangedEventHandler(TreeScope.Subtree)` for icons appearing and disappearing, and
`AddAutomationPropertyChangedEventHandler` on Name (and ToolTip text) for changes such as the clock or
a VPN state. Debounce a burst of events (about 100 ms) and then re-read only the affected subtree. Events
are delivered on a UIA thread, so marshal to our own thread. Re-subscribe if Explorer restarts (the
bridge HWND changes; watch for the TaskbarCreated broadcast). Overflow content has no events until the
flyout exists, so a later overflow design must subscribe when the flyout opens.

## Recommended next step

A supervised follow-up: Invoke the clock or show-desktop button once and confirm it works while
hidden; then, with the user watching, invoke the chevron once, read the overflow window's tree, and
check whether it can be kept offscreen. Until then the dock can show system buttons and any strip
icons, and fall back to the registry list for the rest.
