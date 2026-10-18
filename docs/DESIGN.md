# Halo: research and design

*2026-10-16. Based on reading Seelen UI at
commit `1cad5f1` (2026-10-14) and on a day of building Quiet Dock on this
machine (Windows 11 build 26300, 1920x1200 at 125% + a second monitor).*

## TL;DR

- **Seelen UI** is a Rust backend that does all the Windows integration, plus
  ~25 small web UIs (Svelte/React) shown in WebView2 windows: dock, toolbar,
  start menu, quick settings, flyouts, tiling WM, Alt+Tab, widgets.
- Its **backend is the valuable part**: the same Windows problems exist for any
  engine, and it has solved most of them. Its **front end is where a native app
  wins**: no web runtime, animations on the compositor, live thumbnails instead
  of screenshots, and a dock that actually magnifies.
- Seelen is **AGPL-3.0**. We study approaches and re-implement them; we don't
  copy code.
- **Plan:** a C++/WinRT app drawing with Windows.UI.Composition, built with
  the LLVM-MinGW toolchain already on this PC. A separate tiny **guardian**
  process owns "is the Windows taskbar hidden" so a crash can never leave you
  without a taskbar. The only code that runs inside explorer is an optional,
  read-only tray hook.
- **First step:** a 2-3 day spike (M0) that proves the riskiest parts (blur
  behind a custom-shaped window, magnification driven on the compositor, idle
  cost) before anything else is built.

## What Seelen actually does (from the code)

| Problem | How Seelen solves it | Where |
|---|---|---|
| Hide Windows' taskbar | Sets each taskbar appbar to auto-hide (frees the work area), then `SW_HIDE`; restore = always-on-top + `SW_SHOWNORMAL` | `src/service/shutdown.rs` |
| Never get stuck without a taskbar | A separate **service** process owns the hidden flag and restores the taskbar on shutdown, logoff, or app crash | `src/service/*`, `widgets/weg/mod.rs` |
| Native flyouts fighting its own | WinEvent hook hides ShellExperienceHost notification windows and XAML island flyouts as they appear | `widgets/weg/hook.rs` |
| Reserve screen space | Standard `SHAppBarMessage` (ABM_NEW / SETPOS / SETSTATE) | `windows_api/app_bar.rs` |
| Tray icons | A small DLL hooks **explorer's tray thread** (`WH_CALLWNDPROC`) and copies the `WM_COPYDATA` messages `Shell_NotifyIcon` sends to `Shell_TrayWnd`; read-only, explorer keeps the real tray | `src/hook_dll/src/lib.rs`, `modules/system_tray/` |
| Tray icons that already exist | Broadcasts `TaskbarCreated`; every app re-adds its icons | `system_tray/application/util.rs` |
| Taskbar badges (overlay icons) | Same hook catches `ITaskbarList3` messages to the task band (`WM_USER+79/85`) | `hook_dll` |
| Window tracking | `SetWinEventHook` (out of process), AUMID per window for grouping | `hook.rs`, `windows_api/window/` |
| Sharp icons | `SHGetImageList(SHIL_JUMBO)` = 256x256, crop transparent borders, cache as WebP; UWP via package logos by AUMID | `utils/icon_extractor/` |
| Window previews | Periodic **screenshots** of every window: crop shadow, downscale, hash, WebP-encode, base64, send to the web UI | `modules/apps/application/previews.rs` |
| Start menu data | `.lnk` shortcuts from user + common Start Menu folders, Store apps via `PackageManager.FindPackagesByUserSecurityId` → `GetAppListEntries` | `modules/start/application.rs` |
| Media | `GlobalSystemMediaTransportControlsSessionManager`; audio loopback for a waveform | `modules/media/` |
| Volume / devices | Core Audio: `IMMDeviceEnumerator`, `IAudioEndpointVolume`, per-app sessions | `modules/media/devices/` |
| Wi-Fi / network | WinRT `WiFiAdapter` + `WlanEnum`; `INetworkListManager` for connectivity | `modules/radios/wifi/`, `modules/network/` |
| Bluetooth | WinRT `Radio`, `BluetoothLEDevice`, `DeviceWatcher` | `modules/radios/bluetooth/` |
| Notifications | `UserNotificationListener` | `modules/notifications/` |
| Accent / theme | `UISettings` + `ColorValuesChanged` | `modules/system_settings/` |
| Virtual desktops | `IVirtualDesktop*` (undocumented COM) | `windows_api/mod.rs` |

Its dock ("weg"): pinned + running apps, grouping, window previews, attention
and badge indicators, enter/exit animations. **No magnification**: hover only
tints the item background and brightens the icon (`themes/default/styles/weg.scss`).

Design rules worth adopting (`AGENTS.md`, `documentation/design-system.md`):
tokens only, the user's Windows accent as the only accent, concentric radii
(inner = outer − padding), flat surfaces separated by spacing not borders,
translucency only on dock/toolbar/previews, system font, all strings i18n.

## Feature comparison

| | Seelen UI | Quiet Dock today (Windhawk) | Native app v1 |
|---|---|---|---|
| Runtime | Tauri + WebView2 per surface | Mods inside explorer | One native process + guardian |
| Dock magnification | none | macOS wave, sharp (44px source) | macOS wave, sharp (256px source), on the compositor |
| Dock background follows icons | n/a | yes | yes |
| Window previews | periodic screenshots | Windows' own | live DWM thumbnails |
| Reveal animation | CSS | measured 60fps after fixes | compositor animation |
| Tray | hook DLL mirror | Windows' tray | hook DLL mirror (M3) |
| Toolbar / menu bar | yes, widget-based | no | yes (M2) |
| Quick settings / control center | yes | no | yes (M2) |
| Start menu | yes (pinned, all apps, folders) | Windows' | yes (M4) |
| Tiling WM, Alt+Tab, widgets, wallpapers | yes | no | out of scope |
| Survives its own crash | yes (service restores taskbar) | crash = explorer restart | yes (guardian) |
| Can crash explorer | only via the tray hook | yes (mods run inside it) | only via the tray hook |
| Settings | full settings app | YAML + apply script | YAML, live reload (settings UI later) |

## What to take from Seelen

1. **The guardian.** The hidden-taskbar state lives in a process that does
   almost nothing, so it can't crash with the UI.
2. **Hide via auto-hide + SW_HIDE**, restore by reversing both.
3. **Tray mirroring with a read-only hook** plus the `TaskbarCreated`
   broadcast at startup. Keep the hook tiny, bounds-check everything (it runs
   in explorer), and make it optional.
4. **256px jumbo icons with a disk cache**, AUMID-based grouping, UWP logos.
5. **Start menu data sources** exactly as above.
6. **The API map** in the table: it's a checked list of which Windows API
   backs each toolbar feature.
7. **Design-system discipline**: tokens, concentric radii, system accent.
8. **Lazy module init**: a module (Bluetooth, media...) starts only when a
   surface first needs it.

## What to do differently

- **No web runtime.** Draw with Windows.UI.Composition visuals; text and icons
  rendered once into composition surfaces with Direct2D/DirectWrite.
- **Animations run in the compositor**, not our thread. Magnification becomes
  one property set (`pointerX`) updated on mouse move, plus an expression
  animation per icon (scale and offset from its rest position). DWM evaluates
  them every frame, so a busy UI thread can't drop dock frames.
- **Live previews** with `DwmRegisterThumbnail`: no capture, no encoding, no
  IPC; DWM composites the real window.
- **Event-driven only**: WinEvents, Core Audio callbacks, WinRT events. Nothing
  polls. Target 0% CPU when idle (to be measured, not assumed).
- **Surfaces load on demand**: the start menu and settings don't exist until
  opened.

## Architecture

```
guardian.exe    tiny, no UI. Owns "native taskbar hidden". Starts the shell,
                restores the taskbar if the shell exits or crashes, stops
                restarting after 3 crashes in 2 minutes (safe mode).
shell.exe       all UI. One thread per surface family, each with a
                DispatcherQueue + Compositor + DesktopWindowTarget windows.
                Dock and toolbar per monitor; launcher and control center
                created on demand.
trayhook.dll    optional. Loaded into explorer's tray thread only; copies
                Shell_NotifyIcon traffic to shell.exe over a pipe. Read-only.
config.yaml     every setting; watched and applied live (the workflow you
                already use with apply-portable.py).
```

Reliability rules: no code in explorer except `trayhook.dll` (off by default
until M3 is solid); every Windows event handler catches and logs; the
guardian owns all "restore the user's desktop" state; one hotkey
(e.g. Ctrl+Win+Esc) quits the shell and restores Windows' taskbar instantly.

## Toolchain

C++/WinRT on LLVM-MinGW (clang 20, C++23). It's the compiler Windhawk ships
and it already has the WinRT, Composition, D2D/DWrite, DWM, Core Audio, WLAN,
Radios, media-control and notification headers and import libraries. For the
project we vendor our own copy of the LLVM-MinGW release so the build doesn't
depend on Windhawk being installed. Rust (windows-rs) is the alternative:
safer memory, same APIs, but it needs a toolchain install and the Composition
interop is more verbose.

## Roadmap

| Milestone | Scope | Done when (measured) |
|---|---|---|
| **M0 spike** (2-3 days) | One transparent window: blurred pill behind 15 real app icons (256px), compositor-driven magnification, reveal spring; guardian hides/restores the taskbar | Idle CPU 0% over 60s, hover sweep with no frame over 20ms, RAM recorded; blur works on a custom shape (or we pick a fallback) |
| **M1 dock** | Window tracking, pinned (import Windows' pins), launch/activate/minimize, running dots, multi-monitor, autohide, Windows' taskbar hidden | Daily-drivable for a week without a restart |
| **M2 toolbar + control center** | Mac-style top bar (active app, clock); control center: Wi-Fi, Bluetooth, volume + output device, brightness, media, battery, focus | Every tile backed by the API in the table, no polling |
| **M3 tray + badges** | `trayhook.dll`, tray icons in the toolbar, click/right-click forwarding, overlay badges | Icons match Windows' tray for a week |
| **M4 launcher** | Start menu: search + app grid (Spotlight/Launchpad style), keyboard-first | Opens in under 100ms, finds any installed app |
| **M5 polish** | Live previews, context menus, jump lists, drag-reorder, settings UI | |
| **M6 ship** | Installer, code signing, update channel | |

## Dock decisions (Ziyaad, 2026-10-17)

Look: identical to Quiet Dock, the Windhawk dock it replaces. Logical px:
rest icon 28, peak 44 (157%), cosine wave over 100 px, gap 10, padding 10;
pill 50 tall, radius 20, floating 6 above the edge; glass = blur 40, tint
#16161A at 50%, 2.5% grain, 1px vertical-gradient rim; running dot 4 px white
at 70%. Icons are the stock Windows icons, never masked or restyled. No tray
or clock pill on the dock (a menu bar may come later, separately).

**2026-10-18: light by default.** He asked for "light, airy, aesthetic" across
dock, Start panel and Control Center. All colours now come from `src/theme.h`:
frosted white glass (blur 40, tint #F8F8FB at 84%), dark ink (#1C1C1F), a 1px
black-7% hairline outside the rim, and a soft shadow (black 18%, blur 28, y 8).
Sizes are unchanged. `HALO_THEME=dark` restores the glass described above.
Same day, later: the theme follows the Windows app mode instead (his system is dark, and the white
panel read as wrong), and the Control Center and Start took the dock's hover lift and bounce.
Halo is now the logon shell (HKCU Run → `%LOCALAPPDATA%\Halo\app\guardian.exe`;
ship builds with `tools/install.ps1`), and Quiet Dock's Windhawk mods are disabled.

| Behaviour | Decision |
|---|---|
| Pins | Halo's own list, seeded once from the Windows taskbar pins; drag to reorder, drag off to remove, right-click > Keep in Dock |
| Click on the frontmost app | Minimize it (the Windows habit); other clicks activate, macOS-style |
| Launch | Icon bounces until the app's window appears (max 3) |
| Right-click | Glass menu: the app's jump list items, its open windows, then Keep in Dock / Open at Login / Quit |
| Left end | The Windows Start button |
| Hiding | Intellihide: visible until a window overlaps the dock, spring reveal at the edge |
| Monitors | One dock that follows the cursor to whichever screen's bottom edge it touches |
| Extras | App name label on hover; Recent apps (last 3, after the divider). No badges, no Recycle Bin for now |
| Minimize / restore / maximize | Genie swoop into and out of the app's dock icon: a fork of the MIT "MacOS Minimize Animation" Windhawk mod (`mods/halo-genie.wh.cpp`) that asks Halo where the icon is; maximize gets a matching swoop |

### Control Center and status (Ziyaad, 2026-10-17; supersedes the top-bar concept in CONTROL-CENTER.md)

- **No menu bar.** The Control Center is a floating glass panel at the top right, opened with
  **Win+A** (taken over from Quick Settings while Halo is ON) or by moving the cursor into the
  top-right region of the screen, the way the dock reveals at the bottom. It floats over windows.
- **Status lives in the dock**, at its far left next to the Windows (Start) button: Wi-Fi state
  icon, battery icon + percentage, and the time. Glyphs are the stock Segoe Fluent icons.
- **Tray icons:** the explorer tray hook stays off until it has run a week clean; until then tray
  apps are reached from the Control Center panel.
- Other CONTROL-CENTER.md defaults stand: keep Windows' notification banners, Do Not Disturb via
  IQuietHoursSettings with a Settings fallback.
- **Shortcuts:** Win+Alt+T toggles Halo and the Windows taskbar (guardian); the Windows key alone
  opens Halo's Start panel (its own search; Command Palette is no longer used) and reveals the dock.

## Risks and open questions

- **Blur behind a custom-shaped Win32 composition window.** The options are
  the system backdrop (`DWMWA_SYSTEMBACKDROP_TYPE`), whose shape is the window's,
  or a host backdrop brush in the visual tree. Which works for a pill inside a
  larger transparent window on this build is unverified: the M0 spike decides.
- **Win key → our launcher** without breaking Win+shortcuts needs a
  low-level keyboard hook with careful passthrough (Seelen uses a hotkey
  service). M4.
- **Tray hook** depends on the `Shell_TrayWnd` `WM_COPYDATA` layout (stable
  since XP; Seelen relies on it today) and runs inside explorer, so it is the
  one place a bug can crash explorer.
- **Text input** for search: start with plain typing (WM_CHAR + DirectWrite);
  IME/TSF later.
- **Unsigned binaries** trigger SmartScreen; signing costs money (M6).
- **Scope:** Seelen is years of work. v1 is dock + toolbar + control center +
  launcher, done to a macOS standard, and nothing else.
