# Halo: launch roadmap

*26 Nov 2026. Written from the owner's launch review (`inputs/2026-11-26-launch-review.md`), a check of every claim in it against the source, and a check of every idea in `docs/IDEAS.md`. Code facts carry `path:line` and were confirmed by grep on this date. Anything not confirmed is marked UNVERIFIED with the command that would settle it.*

Line numbers are as of 26 Nov and drift as the code changes; re-run the grep before quoting one.

Halo is public (MIT, 0 stars, no topics, no README images) and its latest GitHub release is v2.0.0 (1 Nov), so the one-line installer gives a stranger a build that is 25 days and 41 commits behind `origin/main` (`git rev-list --count v2.0.0..origin/main`). The review is mostly right about what is missing, but several of its claims describe things that already exist. The order below puts "does not crash and does not lose the tray" ahead of polish and video, because a launch spike only helps if people keep Halo running past day one.

## Status of the 26 Nov review

| Item | Status | Evidence | Priority |
|---|---|---|---|
| Fullscreen / video suppression of the bottom edge | Done (borderless-game behaviour UNVERIFIED) | `src/halo.cpp:1789` SHQueryUserNotificationState; returns on BUSY, D3D fullscreen, presentation mode | n/a |
| Per-monitor DPI v2 awareness | Done | `src/halo.cpp:382-383` | n/a |
| Start and notification panel on DPI change | Missing | `grep -n WM_DPICHANGED src/*.cpp` finds only `src/control_center.cpp:371` | 0 |
| Drag to reorder dock pins | Done | `src/dock.cpp:688` BeginDrag, `:723` EndDrag | n/a |
| Glass grain and rim | Done (top specular on dark glass is not) | `src/halo.cpp:654-657` ("2.5% grain and a 1px rim"); CC grain `src/control_center.cpp:201` | 8 |
| Preview hover delay | Done at 400 ms (review asks 120-180) | `src/halo.cpp:127`, `:1403`; once a strip is open it follows to the next app at once (`ShowPreviewsFor` on hover change) | 8 |
| Genie needs Windhawk, de-friction | Moot | `src/halo.cpp:115` `kLampGenie = false` (owner's choice); mod at `mods/halo-genie.wh.cpp` | n/a |
| Tray icons (VPN, Discord, IME) | Missing | `grep -rni "Shell_NotifyIcon\|NOTIFYICON\|IUIAutomation" src` finds nothing | 1 |
| Taskbar progress bars (ITaskbarList3) | Missing, skip | they go to Explorer's taskbar; Halo cannot see them without hooking Explorer | skip |
| FLASHW attention pulse, badges | Missing | `grep -n "FLASHW\|HSHELL_FLASH\|RegisterShellHookWindow" src/*.cpp` finds nothing | 4 |
| Win+1..9 to pinned order | Missing | no digit handling in `src/winkey.cpp`; `grep -n "VK_NUMPAD\|Win+1" src/*.cpp` finds nothing | 4 |
| Drop a file on a dock icon | Missing | `grep -rn "IDropTarget\|RegisterDragDrop\|DragAcceptFiles" src` finds nothing | 4 |
| First-run 15 s auto-revert card | Missing | `grep -in "revert" src/halo.cpp src/guardian.cpp src/pins.cpp` finds nothing; first run only seeds pins (`src/pins.cpp:100`) | 2 |
| SmartScreen, signing, CI, winget, installer | Missing | no `.github`; only `tools/install.ps1`, `tools/package.ps1`; README says "Code signing is planned" | 2 |
| Uninstall removes startup entry and restores the taskbar | Done | `tools/uninstall.ps1:19-27`; README "Going back to the Windows taskbar" | n/a |
| Retire the "HALO 2.0" name | Missing | `src/settings.cpp:15` key `halo2`; README.md:141 "HALO 2.0 (preview)" | 3 |
| 61-quote widget | Missing (still on) | `src/start_widgets.h:26-88` (61 entries), `:210` `quote = true` | 3 |
| Preferences Reset | Done | `ui/settings.html:687` per section, `:706` and `:323` everything; `src/settings_app.cpp:399` `api/reset`; present since commit 2189a88 | n/a |
| Preferences Export / Import | Missing | `grep -n -i "export\|import" ui/settings.html src/settings_app.cpp` finds nothing relevant | 3 |
| Hide animation timed, reveal is a spring | Missing | `src/halo.cpp:90` `kHideDurationMs = 200`, used at `:1572` | 8 |
| Hairline contrast over bright maximized windows | Missing (UNVERIFIED) | settle with a screenshot over a white maximized browser | 8 |
| Developer mode (logo opens the Win+X menu) | Built, not installed | commit 096f3e7; `src/settings.cpp:16`, `src/halo.cpp:1887`, `src/winkey.h:17` | 7 |
| Hero video, fresh benchmarks, ARCHITECTURE.md, topics | Missing | `ls docs` has no ARCHITECTURE.md; README has 0 images | 9 |
| Upstream MIT text for the genie fork | Done, one question open | `THIRD-PARTY-NOTICES.md` (upstream v3.1.6, ramensoftware/windhawk-mods@1cefd9b, full MIT text). Open: Potassiumuncher, credited upstream for the D2D engine, has no licence statement of his own (his repo's licence is null); ask him to confirm MIT | 2 |
| README memory number | Stale | README.md:45-47 says 35.5-37.2 MB (v2.1.0, 18 Nov); ops LOG measured 43-51 MB on 23 Nov. README also names v2.1.0, but the latest release is v2.0.0 (`gh release list -R ziyaad-mallick/halo`) | 9 |

Crashes in `%LOCALAPPDATA%\Halo\crash.log` (4 records since 16 Nov, `grep -n "kind=" crash.log`):
- 22 and 25 Nov: null read during WM_POWERBROADCAST after "pill shadow: FAILED 0x80131509". Fixed in 096f3e7 (`src/halo.cpp:560-566` MakeSurface throws; `:1831` exit code 3 so the guardian restarts Halo).
- Open: 24 Nov heap corruption 0xC0000374 under crumb kMsgSettings (`src/halo.cpp:1849`); its stack passes through Windows.Media.MediaControl.dll.
- Open: 16 Nov access violation under crumb `tracker.onForeground` (`src/halo.cpp:464`, `src/apps.cpp:184`).

## Roadmap (in order)

Sizes: S under a day, M a few days, L a week or more.

### 0. Crash-free first
- **What:** root-cause the 24 Nov heap corruption during a settings reload (suspect the media session handler running while `kMsgSettings` is processed) and the 16 Nov null read in `tracker.onForeground`. Install 096f3e7 and watch. Also handle WM_DPICHANGED in Start and the notification panel (S; pattern at `src/control_center.cpp:371`).
- **Why:** a shell that crashes is uninstalled on day one. Two of four logged crashes are still open.
- **Size:** M.
- **Done when:** `crash.log` has no new record for 7 consecutive days on the installed build including sleep/resume cycles; toggling a setting 200 times leaves no record; dragging Start and the notification panel between a 150% and a 100% monitor leaves no stale scale (screenshot pair).

### 1. System tray
- **What:** 1-day spike reading and invoking the hidden taskbar's tray through UI Automation, with no injection into Explorer. If it works, a dock chevron plus a glass flyout. A hook DLL only as a fallback.
- **Why:** hiding the taskbar hides VPN, Discord, OBS, Steam and the IME indicator, the main reason users go back. `docs/IDEAS.md` rated the tray a poor fit because of Explorer injection; UI Automation removes that objection.
- **Size:** spike S, flyout M. Whether UI Automation can reach the hidden tray is UNVERIFIED; the spike settles it.
- **Done when:** with Discord, a VPN client and the keyboard indicator running, each appears in the flyout and left and right click reach the app, and no Halo module is loaded into explorer.exe (`tasklist /m /fi "imagename eq explorer.exe"`).

### 2. Trust and install
- **What:** first-run 15 s auto-revert card (Win+Alt+T and Ctrl+Win+Esc hints; doing nothing restores the taskbar); safety section at the top of README; GitHub Actions release builds; SignPath Foundation application; winget manifest; THIRD-PARTY-NOTICES.md; a fresh release so "latest" is current.
- **Why:** strangers will not run an unsigned script that hides their taskbar without guardrails. The guardian is already the best answer and is buried in the README.
- **Size:** L in total (card M, CI S; signing and winget wait on outside approval).
- **Done when:** on a clean Windows 11 VM, no input for 15 s ends with the taskbar back; the release zip comes from a public Actions run and its SHA-256 matches the published one; `winget install` works from a local manifest; `gh release list` shows a release newer than 1 Nov.

### 3. One product
- **What:** rename HALO 2.0 to Layout: Classic / Dashboard (settings key, UI, README, `docs/SETTINGS.md`); quote off by default with the slot showing the next calendar item or now playing; Preferences Export / Import of `settings.ini` (Reset already exists). Classic Start is now Pinned (12), Recent files (10), Recent apps (6), with Start's own pin list and right-click Pin to Start / Unpin (built on `launch/dev-mode`, `src/start_widgets.h` PlanClassic, checked by `start_test`; not installed yet). Classic must stay: another user relies on it.
- **Why:** a version-named toggle reads as an unfinished branch.
- **Size:** M.
- **Done when:** `grep -rni "halo 2.0\|halo2" ui README.md docs` finds only a migration note; an old `halo2 = on` file still loads into the Dashboard layout; export then import on a fresh profile reproduces the file byte for byte.

### 4. Taskbar parity
- **What:** Win+1..9 to pinned order; attention pulse via RegisterShellHookWindow (HSHELL_FLASH); drop a file on a dock icon to open with it (IDropTarget); dock jump lists (ICustomDestinationList) beside the existing right-click menu (`src/halo.cpp:1241`).
- **Why:** keyboard and drag habits are what let a dock replace a taskbar. Progress bars are skipped (see table).
- **Size:** M.
- **Done when:** Win+3 launches or focuses the third pin; a flashing Notepad pulses its icon; dropping a .txt on the Notepad icon opens it; right-click on a pinned app lists its recent items.

### 5. Control Center
- **What:** inline audio-output switcher, per-app volume, Focus with a timer and per-app notification mute, keyboard layout / IME switcher, tray flyout entry, user-chosen and reordered tiles.
- **Why:** it is the best-looking surface. Media and volume exist (`src/services.cpp:53`, `:173`, `:2143`), but Do Not Disturb is a single toggle of the toast registry value (`src/services.cpp:543-546`, `src/control_center.cpp:1112-1114`).
- **Size:** L.
- **Done when:** switching output device, a 30 minute Focus that ends by itself, and muting one app's toasts all work without leaving the panel.

### 6. Overview and four hot corners (before the video)
- **What:** Overview shows every window on the current monitor as a zoomable grid of live DWM thumbnails with compositor motion, thumbnails registered only while it is open. Four assignable hot corners (Overview, Start, Control Center, show desktop) extend today's single hot zone.
- **Why:** live thumbnails are the moat (competitors use screenshots, per IDEAS.md) and Overview is the hero shot of the launch video. Thumbnail code exists (`src/previews.cpp:140`, `src/swoop.cpp:311`).
- **Size:** Overview M-L; corners S (`src/edge.cpp:47` InCorner, `src/edge.h:22` Configure).
- **Done when:** first frame within 100 ms of the trigger at 10 open windows (target; measure with `tools/framedump.cpp`); no thumbnail stays registered after close; each corner fires only after its dwell.

### 7. Customisation beyond macOS
- **What:** developer mode (done, 096f3e7); dock items that run scripts, URLs or commands; per-app dock rules; shareable theme files (builds on Export / Import); ARCHITECTURE.md for widget authors.
- **Size:** M-L.
- **Done when:** a theme file exported on one machine applies on another, and a pinned script item runs on click.

### 8. Motion polish
- **What:** preview delay 400 to about 200 ms (`src/halo.cpp:127`); hide as an interruptible spring instead of the 200 ms keyframe (`src/halo.cpp:90`, `:1572`); top specular rim in dark glass; hairline contrast over bright maximized windows.
- **Size:** M.
- **Done when:** a flick away and back mid-hide shows no velocity jump in a frame capture, and the hairline reads clearly over a white maximized window.

### 9. Launch assets
- **What:** 60 fps MP4 hero (Overview, magnification, Control Center, Task Manager beside it); fresh benchmark table with Windows build, CPU/GPU and method; ARCHITECTURE.md (guardian / halo / settings IPC); repo topics and screenshots.
- **Size:** M.
- **Done when:** every number in the README is reproduced by a command run on the release tag (README.md:45-47 is stale), and the README has the video and at least three images.

### After launch
Quick Look via PowerToys Peek when installed (S; native M); dock stacks (M, demand unproven); calculator in Start search (S); calendar events (M, blocked on package identity, see the ideas table).

## docs/IDEAS.md, checked and ranked (26 Nov)

| # | Idea | Status today | Evidence | Rank tier | Where it lands |
|---|---|---|---|---|---|
| 1 | Hot corners | Partial: one zone | `src/edge.h:4` (right fifth of the bottom edge), `:22` Configure; `src/settings.cpp:45-47`; `src/edge.cpp:47` | A | Step 6 |
| 2 | Overview / Mission Control | Missing | no overview code; thumbnails only in previews and swoop (`src/previews.cpp:140`) | A | Step 6 |
| 3 | Dock stacks | Missing | `grep -n ReadDirectoryChangesW src/*.cpp` finds nothing | B | After launch |
| 4 | Quick Look | Missing | `grep -n -i "peek" src/launcher.cpp` finds nothing | B | After launch |
| 5 | Snap layout picker | Missing | `grep -rni "snap layout" src` finds nothing | C, skip | Windows 11 has Snap Layouts |
| 6 | Launcher calc, units, clipboard | Missing | only a Settings shortcut named Clipboard, `src/launcher.cpp:403` | B (calc only) | After launch; skip clipboard history (Win+V exists) |
| 7 | Named window groups | Missing | no code | C, skip | L, fragile, polarizing |
| 8 | Notifications and Focus | Partial | DND toggle `src/control_center.cpp:934`, `:1112`, `src/services.cpp:543`; no timer or per-app mute (`grep -n -i "mute" src/notif*.cpp` matches only the theme::Muted text colour) | A | Step 5 |
| 9 | CC media and volume | Done | `src/services.cpp:53`, `:173`, `:2143` | n/a | n/a |
| 10 | Dock quick actions | Partial | right-click menu `src/halo.cpp:1241`; no `IDropTarget`, `RegisterDragDrop` or `ICustomDestinationList` anywhere in `src` | A | Step 4 |
| 11 | Calendar events | Partial: month grid only | `grep -n -i "AppointmentStore" src` finds nothing. Appointment access needing package identity is UNVERIFIED; settle with an unpackaged test of `AppointmentManager::RequestStoreAsync` | B | After launch; tied to the installer decision in step 2 |
| 12 | Gestures | Missing | no Raw Input touchpad code | C, skip | Windows has presets |
| 13 | Phone share | Missing | no code | C, skip | Microsoft is building it |
| 14 | Launchpad grid | Partial | All apps is a list: `src/launcher.h:99` Kind::AllApps, `:149` ToggleAllApps | C, skip | exists |
| 15 | Tray / menu-bar extras | Missing | no `NOTIFYICON` or UI Automation in `src`; IDEAS.md rejected it for Explorer injection | A | Step 1 |

Tier A is built before launch, B after, C is not planned. Grep false positives (PeekMessage, snapshot, calculate) were ignored.

## Not planned
- **Progress bars from apps.** They reach Explorer's taskbar, not Halo.
- **Explorer injection** as the first choice for the tray.
- **Windhawk de-friction.** The genie is off by default (`src/halo.cpp:115`).
