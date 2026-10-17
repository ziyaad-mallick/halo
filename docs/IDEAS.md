# Halo: what to build next (ranked)

*2026-10-17. Research pass, web search only. Reddit threads did not surface in
search, so user-complaint evidence comes from forums, Steam, GitHub, news and
Seelen's own known-issues page. Treat "users want it" as a hypothesis to check.*

Effort is for C++/WinRT + Windows.UI.Composition. "Idle" = zero CPU when nothing
is happening; the rule is event-driven only (no timers, no polling).

## Top 5

### 1. Hot corners (corner actions: Task View, show desktop, Halo launcher, Control Center)
- **What:** Assign actions to the four screen corners. Halo already owns an edge flick for Control Center, so corners are the natural sibling.
- **Why users want it:** Windows 11 has no built-in hot corners and Microsoft offers no settings page for them. Users reach for third-party apps: [HotCornersWin](https://github.com/flexits/HotCornersWin), [WinXCorners](https://www.softpedia.com/get/System/OS-Enhancements/WinXCorners.shtml), [Charmy](https://betanews.com/series/best-windows-apps-this-week-260) (which has no custom actions yet), and [geekchamp guide](https://geekchamp.com/?p=760322) (low quality).
- **Effort:** S. Corner-hit via low-level mouse hook or the existing edge logic; no timer.
- **Risks:** Low-level mouse hooks can add input latency if slow; keep the callback trivial. Avoid firing on drags into corners (needs a dwell delay).
- **Fits bar?** Yes. Event-driven, no idle cost.

### 2. Mission Control overview with live DWM thumbnails
- **What:** One key or corner shows every window on the current monitor (or all desktops) as a zoomable grid, with live thumbnails.
- **Why:** Windows' Task View is the closest match, but it is a coarse grid. macOS users name Mission Control as the feature they miss most in [TechRadar's "missed from Windows" piece](https://www.techradar.com/computing/software/i-started-using-a-mac-full-time-for-work-but-these-are-the-things-i-miss-from-windows-11-that-made-me-switch-back) and in [Tom's Guide](https://www.tomsguide.com/computing/macbooks/switched-to-mac-these-5-missing-windows-features-are-actually-hiding-in-plain-sight). Seelen's previews are **periodic screenshots** (per Halo's own DESIGN.md), which is the gap: DWM thumbnails are live and GPU-side.
- **Effort:** M. `DwmRegisterThumbnail` plus Composition animation for the zoom.
- **Risks:** Thumbnails of some protected or elevated windows fail; show a fallback icon. Multi-desktop listing needs undocumented virtual-desktop COM (same risk Seelen already carries).
- **Fits bar?** Yes if thumbnails exist only while the overlay is open. Must not keep thumbnails registered when hidden.

### 3. Dock stacks (Downloads and folder stacks that fan out)
- **What:** A folder pinned to the dock opens as a fan or grid of its contents, like macOS Dock stacks.
- **Why:** Dock stacks are a macOS feature with no native Windows equivalent; the taskbar has only jump lists. Confirm demand before building: I found no direct complaint thread. Evidence is indirect (the Mac-switcher articles above list dock-side conveniences as things missed).
- **Effort:** M. Folder enumeration plus `ReadDirectoryChangesW` (event-driven, so no polling).
- **Risks:** Large folders need virtualisation. Shell thumbnails (`IShellItemImageFactory`) can be slow on network drives.
- **Fits bar?** Yes. Watch-based, no idle cost.

### 4. Quick Look on Space: built-in preview for launcher and stack results
- **What:** Press Space on a highlighted file in the Win-key launcher, a dock stack, or Halo's recent-files list to get a fast preview. Use Windows Imaging Component for images and a simple text/PDF path for others.
- **Why:** Quick Look is a macOS staple. Windows' answer is [PowerToys Peek](https://www.neowin.net/news/windows-11-just-borrowed-a-great-productivity-tool-from-mac/), which since 0.95 can use a single Space key. But Peek is a separate app, and its default is still Ctrl+Space ([Neowin](https://www.neowin.net/news/windows-11-just-borrowed-a-great-productivity-tool-from-mac/)). The gap is an integrated preview inside Halo's own surfaces.
- **Effort:** S (delegate to Peek if installed); M (native preview).
- **Risks:** Preview handlers run in-process if you call them, so a bad codec can crash Halo. Run heavy previews out of process or limit to WIC/text.
- **Fits bar?** Yes, on demand only.

### 5. Snap-layout picker from a dock or hotkey (the macOS green-button menu, without hooking other apps)
- **What:** Open a small layout picker (halves, thirds, quarters, custom zones) from the dock or a hotkey, then apply it to the focused window.
- **Why:** macOS Sequoia added tiling via the green button, but [sources in the research](https://windowsnews.ai/article/5-windows-11-features-that-macos-still-cant-match-and-what-mac-users-can-do-about-it.440408) note Windows snapping is ahead. So this is a *polish* gap, not a gap in capability. Users who want custom zones already use [FancyZones](https://www.tomsguide.com/computing/macbooks/switched-to-mac-these-5-missing-windows-features-are-actually-hiding-in-plain-sight). The gap is a *discoverable* picker, which is also why I ranked it below items 1–4.
- **Effort:** M. Hooking the maximize button of other apps would be L and fragile, so don't.
- **Risks:** Some apps have minimum sizes that break tiling. Snap-layout APIs are partly undocumented.
- **Fits bar?** Yes.

## Ranks 6–15

| # | Idea | Why (source) | Effort | Risks | Fits bar? |
|---|---|---|---|---|---|
| 6 | Launcher with calc, unit conversion, clipboard history and file preview, all resident-free | Launchers are the most-compared category: [Flow Launcher, PowerToys CmdPal, Raycast beta](https://zenn.dev/yahat/articles/c928bf7a5d1fff?locale=en). The same comparison claims Raycast uses ~300–500 MB (figures look unreliable, so verify). Gap: a launcher that costs nothing when closed. | M | Extension API is a security surface; keep it built-in only | Yes if it is hotkey-launched, not resident |
| 7 | Named window groups (Stage Manager-style) | A [StageManager](https://awesome.ecosyste.ms/projects/github.com%2Fawaescher%2Fstagemanager) prototype exists but is unmaintained. PCWorld user wants named groups. | L | Moving windows off-screen is fragile with DWM; Stage Manager is polarizing | Partly |
| 8 | Notification and Focus modes (per-app mute, DND schedule) | Seelen notification fixes in its changelog ([v2.4.3](https://seelen.io/apps/seelen-ui/releases/latest)) show this is still rough. Source: `UserNotificationListener`. | S–M | Listener needs the right capability | Yes |
| 9 | Control Center media and volume tiles (GSMTC + Core Audio) | Seelen already does media via GSMTC; Halo's Control Center is the natural home. | S | Low | Yes |
| 10 | Dock item quick actions and recents (right-click lists recent files, drag files onto icons to open) | Expected dock behaviour; MyDockFinder users describe dock-to-minimize issues ([Steam](https://store.steampowered.com/app/1787090/MyDockFinder/)), so getting this right is a differentiator. | S | Drop targets need OLE drag-drop | Yes |
| 11 | Calendar flyout in the status cluster (next events, Outlook or Google) | Status-cluster clock is an obvious surface. | M | OAuth or Graph auth is heavy; start with local calendar only | Yes |
| 12 | Gesture customisation beyond Settings (3/4-finger swipes to Mission Control / desktops) | Windows already exposes [touchpad gesture presets](https://www.laptopmag.com/how-to/how-to-customize-your-touchpad-in-windows-11), so this is a smaller gap. | M | Raw Input touchpad parsing is fiddly per vendor | Yes |
| 13 | Clipboard and file send to phone (AirDrop-style, via Phone Link or Nearby Share) | Microsoft is building an [AirDrop-like feature](https://www.techradar.com/computing/windows/good-news-microsoft-is-finally-realizing-a-feature-resembling-airdrop-for-windows-pc-and-android-device-users); [Phone Link share sheet](https://windowscentral.com/software-apps/windows-11/microsofts-airdrop-alternative-for-android-is-baked-right-into-windows-11-heres-how-to-get-it) already covers it. Overlap is high. | L | Undocumented Nearby Share protocol; platform owner may ship it first | Weak fit; defer |
| 14 | Full Start-menu replacement grid (Launchpad-style) | ExplorerPatcher and StartAllBack were [blocked on 24H2](https://www.neowin.com/news/tags/explorerpatcher/); users on 24H2 report breakage ([DeskModder forum](https://www.deskmodder.de/phpBB3/viewtopic.php?p=429586)). Shows demand and how risky shell replacement is. | L | Shell and Start internals change every build; very high scope | Poor fit; the risk is the point |
| 15 | Menu-bar extras (tray icons hosted in Halo's status cluster) | Seelen does this with a DLL hook into explorer's tray ([Seelen docs](https://seelen.io/blog/known-issues)). | L | Explorer injection is a security and stability risk (Smart App Control already blocks unsigned Halo builds) | Poor fit |

## Things to avoid or treat with care

- **Explorer injection** (tray, badges): Seelen needs it, and Halo's guardian design deliberately avoids it. Keep it that way.
- **Hooking other apps' title bars** (green-button clone): fragile across apps and needs more privilege than it is worth.
- **Anything with a timer**: hot corners and previews must be event-driven, or they break the zero-idle bar.
- **Undocumented COM for virtual desktops**: Seelen uses it; expect breakage on each Windows feature update.

## Known issues in comparable projects (for the "where users hurt" list)

- **Seelen UI:** install failures with multiple installs or the Store version; tray empty on some setups; gray boxes from other shell mods; higher memory use ([Seelen known issues](https://seelen.io/blog/known-issues), [touchit.sk review](https://touchit.sk/seelen-ui/847590/)).
- **MyDockFinder:** Win+D flicker (developer says it is "nearly unsolvable"); minimize-to-dock issues; a 24H2 "Bad Image" error for dockmod64.dll reported Feb 2026 ([Steam discussion](https://steamcommunity.com/app/1787090/discussions/0/595140733594266095/?l=latam)). Developer updates take weeks.
- **Windhawk taskbar mods:** the thumbnail-disable mod broke drag-and-drop until 1.1.1 ([ramensoftware](https://ramensoftware.com/?p=3134)).
- **ExplorerPatcher / StartAllBack:** 22H2 startup loop (Feb 2023, [The Register](https://www.theregister.com/2023/03/01/microsoft_windows_11_crash)); 24H2 block ([Neowin](https://www.neowin.com/news/tags/explorerpatcher/)).
- **YASB / GlazeWM / Zebar:** install channels unclear (GlazeWM README contradicts itself on Winget/Scoop) ([GitHub](https://github.com/glzr-io/glazewm), [YASB](https://github.com/amnweb/yasb)).

## Gaps Halo can own

1. **Live, not screenshot, previews and overview** (items 1, 2): Seelen-style screenshots are the weak point.
2. **Zero idle CPU as a stated feature.** The competitors above are Rust/WebView or Python apps; few advertise idle cost. Measure and publish it.
3. **Predictable on updates:** keep to documented APIs where possible, and guard undocumented COM behind feature checks.
4. **macOS-native feel from the compositor:** magnification and springs already exist; extend the same motion language to overview and stacks.

## Sources

- [Seelen known issues](https://seelen.io/blog/known-issues), [Seelen FAQ](https://seelen.io/blog/seelen-faq), [touchit.sk review](https://touchit.sk/seelen-ui/847590/)
- [TechRadar, missed Windows features](https://www.techradar.com/computing/software/i-started-using-a-mac-full-time-for-work-but-these-are-the-things-i-miss-from-windows-11-that-made-me-switch-back), [Tom's Guide](https://www.tomsguide.com/computing/macbooks/switched-to-mac-these-5-missing-windows-features-are-actually-hiding-in-plain-sight), [windowsnews.ai](https://windowsnews.ai/article/5-windows-11-features-that-macos-still-cant-match-and-what-mac-users-can-do-about-it.440408)
- [HotCornersWin](https://github.com/flexits/HotCornersWin), [WinXCorners](https://www.softpedia.com/get/System/OS-Enhancements/WinXCorners.shtml), [BetaNews apps roundup](https://betanews.com/series/best-windows-apps-this-week-260)
- [Neowin, PowerToys Peek](https://www.neowin.net/news/windows-11-just-borrowed-a-great-productivity-tool-from-mac/)
- [Launcher comparison (zenn.dev)](https://zenn.dev/yahat/articles/c928bf7a5d1fff?locale=en), [SmartWorld, Command Palette](https://www.smartworld.it/news/windows-11-powertoys-command-palette-velocita.html)
- [MyDockFinder on Steam](https://store.steampowered.com/app/1787090/MyDockFinder/), [Steam discussion](https://steamcommunity.com/app/1787090/discussions/0/595140733594266095/?l=latam)
- [Windhawk Disable Taskbar Thumbnails](https://ramensoftware.com/?p=3134)
- [DeskModder ExplorerPatcher/StartAllBack forum](https://www.deskmodder.de/phpBB3/viewtopic.php?p=429586), [Neowin ExplorerPatcher tag](https://www.neowin.com/news/tags/explorerpatcher/), [The Register, Feb 2023](https://www.theregister.com/2023/03/01/microsoft_windows_11_crash)
- [GlazeWM](https://github.com/glzr-io/glazewm), [YASB](https://github.com/amnweb/yasb)
- [StageManager prototype listing](https://awesome.ecosyste.ms/projects/github.com%2Fawaescher%2Fstagemanager)
- [Phone Link share sheet (Windows Central)](https://windowscentral.com/software-apps/windows-11/microsofts-airdrop-alternative-for-android-is-baked-right-into-windows-11-heres-how-to-get-it), [TechRadar AirDrop-like feature](https://www.techradar.com/computing/windows/good-news-microsoft-is-finally-realizing-a-feature-resembling-airdrop-for-windows-pc-and-android-device-users)
- [Touchpad gestures (LaptopMag)](https://www.laptopmag.com/how-to/how-to-customize-your-touchpad-in-windows-11)
