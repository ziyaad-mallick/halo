# Halo Control Center: research and design

*2026-10-17. Design document, no product code. Companion to [DESIGN.md](DESIGN.md) (roadmap M2/M3).
`S:` below = a local clone of the AGPL [Seelen UI](https://github.com/eythaann/Seelen-UI).
Studied, never copied.*

Labels: **VERIFIED** = read in Seelen code or on Microsoft Learn (URL/path given); **KNOWN** = standard
documented API from memory, not re-fetched today; **UNVERIFIED** = test before relying.

## 0. Constraints

| Fact | Consequence |
|---|---|
| Dock at the bottom replaces the taskbar; Windows tray and clock are hidden | Tray icons, clock, calendar and notifications need a new home |
| Search/launcher out of scope | No Spotlight icon, no Win-key work |
| 1920x1200 @125% (1536x960 logical) + sometimes 1920x1080 on the left | A top bar costs 24 of 960 logical px (2.5%); the panel must fit the 1080p monitor too |
| 0 ms idle CPU, no polling | Every module is an event subscription, or is read only while its panel is open |
| Unpackaged exe | No package identity: see 3.2 |

## 1. What macOS does (Sonoma / Sequoia / Tahoe)

### 1.1 Menu bar, right side (right to left)

| Item | Behaviour |
|---|---|
| Clock | "Fri 17 Oct 9:41 AM"; click opens Notification Center (notifications + widgets) |
| Control Center icon (two toggles) | Click opens the panel |
| Third-party status items | Small monochrome icons, click opens a popover or menu; Cmd-drag reorders/removes. No overflow UI (hence Bartender-style apps; they hide behind the notch) |
| Built-in items | Wi-Fi, Bluetooth, battery, sound, Focus, display, Now Playing: each can be pinned via System Settings > Control Center ("Show in Menu Bar"), or by dragging a module out of the panel |

Left side: Apple menu, active app name (bold), app menus. Bar is 24 pt (taller on notched Macs), translucent, tinted from the wallpaper (Tahoe: fully transparent). It hides in full screen and reveals when the cursor hits the top edge.

### 1.2 Control Center panel

Roughly 320 pt wide, anchored under the icon, ~18-20 pt radius, vibrant material, grid of square and wide modules.

```
+------------------------------------------+
| [ Wi-Fi | Bluetooth | AirDrop  2x2 ]  [Focus]   |
|                                  [Stage Mgr][Screen Mirroring]|
| Display   ========O=====   (sun)         |
| Sound     =====O========   (speaker) (>) |
| [ Now Playing: art, title, prev/play/next ]|
| [Keyboard Brightness]  [Battery / Low Power]|
+------------------------------------------+
```

| Module | Collapsed | Expanded (click the label/chevron) |
|---|---|---|
| Wi-Fi | round toggle + SSID | network list, "Wi-Fi Settings..." |
| Bluetooth | toggle + device count | device list with battery, click to connect/disconnect |
| AirDrop | toggle | Off / Contacts Only / Everyone |
| Focus | tile showing current mode | list of modes + durations ("For 1 hour", "Until tonight") |
| Display | slider | + Dark Mode, Night Shift, True Tone |
| Sound | slider | output device picker (checkmark on current) |
| Now Playing | art, title, transport | none; click art opens the app |
| Screen Mirroring | tile | device list |
| Battery | tile | Low Power Mode, apps using significant energy |

Interactions: sliders are ~28 pt-tall pills whose filled part is the thumb; toggles are round glyph buttons that fill with accent when on; the panel closes on click-outside, Esc or app switch; modules can be dragged into the menu bar. Motion: panel appears from its anchor in ~250 ms (small scale-up + fade, no visible bounce); a module expands in place, siblings cross-fade and the container height animates with a damped spring; sliders track the pointer 1:1. Sequoia/Tahoe let the user choose which modules show; Tahoe adds the Liquid Glass material and rearrangeable layout (UNVERIFIED in detail, recalled from the announcement).

### 1.3 Notification Center and widgets

Click the clock: a right-edge sheet (~ 360 pt) slides in (~ 300 ms): notification groups on top (grouped by app, X to clear, "Clear All"), widgets below (calendar, weather, reminders), "Edit Widgets" at the bottom. Banners appear top-right, auto-dismiss in ~ 5 s, hover reveals actions.

## 2. Where it lives on Windows

All concepts keep the dock at the bottom and use the same glass material.

### Concept A: slim top menu bar with status items + clock

```
+--------------------------------------------------------------------------------+
| Chrome                       Discord OneDrive Steam  wifi  batt  [CC]  Fri 17 Oct 9:41 |
+--------------------------------------------------------------------------------+
|                                                                  +-------------+
|                        (app content)                             |Control Ctr  |
|                                                                  |(4-col grid) |
|                       [dock icons .............. ]               +-------------+
+--------------------------------------------------------------------------------+
```

| Aspect | Notes |
|---|---|
| Screen space | Reserves 24 px via `SHAppBarMessage` (ABM_NEW / ABM_SETPOS, edge top), as Seelen does (`S:src/background/windows_api/app_bar.rs`); maximised windows respect it |
| Discoverability | Best: clock, battery, Wi-Fi always visible; the exact macOS answer |
| Tray icons | Natural home: right of the bar, overflow chevron beyond ~ 8 |
| Multi-monitor | One bar per monitor, clock on all, tray items on the primary only |
| Fullscreen | Bar and dock hide on `ABN_FULLSCREENAPP`; optional top-edge reveal |
| Cost | One more permanent window (must stay 0 ms). The left side cannot have real app menus (Windows has no global menu), so it is just the app name |

### Concept B: no bar; hot corner or hotkey opens a floating panel

```
+--------------------------------------------------------------------------------+
|                                                           [hot zone]           |
|                                                  +----------------------+      |
|                       (app content)              | CC + tray grid       |      |
|                                                  +----------------------+      |
|                       [dock icons .............. ]                              |
+--------------------------------------------------------------------------------+
```

| Aspect | Notes |
|---|---|
| Screen space | Zero while closed |
| Discoverability | Poor: no clock or battery unless the dock grows a status tail; top-right hot corner collides with app close buttons |
| Tray icons | Hidden in a grid; attention states (Discord unread, a call) are invisible |
| Multi-monitor | Corner on a shared edge between monitors is ambiguous |
| Fullscreen | Works well |

### Concept C: compact glass pill in the top-right corner

```
+--------------------------------------------------------------------------------+
|                                                   [ wifi  bt  87%  9:41 ]      |
|                       (app content, full height)                               |
|                       [dock icons .............. ]                              |
+--------------------------------------------------------------------------------+
```

| Aspect | Notes |
|---|---|
| Screen space | None reserved, but it overlaps the top-right of maximised windows (caption buttons). Fixing that means reserving a strip, which makes it a worse A |
| Discoverability | Good for clock/battery |
| Tray icons | A pill holds 3-4 glyphs; the rest go behind a popover |
| Multi-monitor / fullscreen | Same as A |

### Recommendation: Concept A, slim

- A 24 px glass bar on every monitor: focused app's name on the left (no fake menus), on the right tray icons, Wi-Fi, battery, Control Center icon, clock.
- Control Center panel (icon) and Notification Center sheet (clock) are separate on-demand windows.
- Why: the tray and clock must live somewhere permanent, a reserved strip does not cover caption buttons, and it matches the macOS reference.
- Add a setting to auto-hide the bar (off by default); that gives Concept B behaviour without a rewrite.

## 3. Module catalogue

Status: Public = documented; Undoc = undocumented/reverse-engineered. Seelen paths are evidence, not code to copy.

### 3.1 Table

| Module | Read state | Change notification (no polling) | Change | Status | Verdict |
|---|---|---|---|---|---|
| Clock/calendar | `GetLocalTime`, `GetDynamicTimeZoneInformation` | One waitable timer to the next minute boundary; `WM_TIMECHANGE`, `WM_SETTINGCHANGE` for tz/format | n/a | Public (KNOWN) | **MVP** |
| Battery | `GetSystemPowerStatus` (Seelen: `S:src/background/windows_api/mod.rs:1062`) | `PowerSettingRegisterNotification` with `GUID_BATTERY_PERCENTAGE_REMAINING`, `GUID_ACDC_POWER_SOURCE` (Seelen registers via `PowerSettingRegisterNotification`, `S:src/background/modules/power/application.rs:10`) | n/a | Public | **MVP** |
| Power mode (Best performance / Balanced / Efficiency) | `PowerGetEffectiveOverlayScheme` (KNOWN) | `PowerRegisterForEffectivePowerModeNotifications` (VERIFIED `S:.../modules/power/application.rs:92`) | `PowerSetActiveOverlayScheme(GUID)` in powrprof; no Learn page found today (404) | Read/notify Public; set UNVERIFIED | **MVP** read; set only if it works on build 26300, else link to Settings |
| Volume + mute | `IAudioEndpointVolume` | `IAudioEndpointVolumeCallback` (VERIFIED `S:.../modules/media/devices/application.rs:9,400`) | `SetMasterVolumeLevelScalar`, `SetMute` | Public | **MVP** |
| Output device list + switch | `IMMDeviceEnumerator::EnumAudioEndpoints`, `GetDefaultAudioEndpoint` | `IMMNotificationClient` via `RegisterEndpointNotificationCallback` (VERIFIED `application.rs:115`) | `IPolicyConfig::SetDefaultEndpoint` for each role (CLSID 870af99c-171d-4f9e-af0d-e63df40c2bc9, IID f8679f50-850a-41cf-9c72-430f290290c8; VERIFIED in `S:src/background/windows_api/undocumented/audio_policy_config.rs:15-21`) | Read Public; **set Undoc** | **MVP**; on COM failure fall back to `ms-settings:sound` |
| Per-app volume | `IAudioSessionManager2` enumerator, `ISimpleAudioVolume` | `IAudioSessionNotification` for new sessions (VERIFIED import `application.rs:12`), session events for changes | `ISimpleAudioVolume::SetMasterVolume` | Public | Later (second level of Sound) |
| Microphone mute | Capture endpoint `IAudioEndpointVolume::GetMute` | Same callback on the capture endpoint | `SetMute` | Public | **MVP** |
| Wi-Fi (state, list, connect) | `WiFiAdapter.NetworkReport`; `NetworkInformation.GetConnectionProfiles()` (VERIFIED `S:.../radios/wifi/adapter.rs:39`) | `WiFiAdapter.AvailableNetworksChanged`, `NetworkInformation.NetworkStatusChanged`, `Radio.StateChanged` | `ScanAsync`, `ConnectAsync(network, kind, PasswordCredential)` (VERIFIED `S:.../radios/wifi/mod.rs:277`), disconnect, `Radio.SetStateAsync`. Hidden SSIDs and 802.1X need `WlanSetProfile` (Win32 wlanapi, as Seelen uses `WlanGetAvailableNetworkList`) | Public | **MVP** (toggle, SSID, list, connect to known/open/PSK); enterprise -> Settings |
| Bluetooth toggle + devices | `Radio.GetRadiosAsync` (Kind Bluetooth); `DeviceInformation` with `BluetoothDevice.GetDeviceSelectorFromPairingState(true)` | `Radio.StateChanged`, `DeviceWatcher` (VERIFIED `S:.../radios/manager.rs:4`, `device.rs:27`) | `Radio.SetStateAsync` after `Radio.RequestAccessAsync` (VERIFIED `manager.rs:97`). Connect/disconnect a paired device: no public API found | Toggle Public; connect none | **MVP** toggle + device list; connect action Later (open Settings meanwhile) |
| Bluetooth device battery | Device property for battery % (`{104EA319-6EE2-4701-BD47-8DDBF425BBE5}` pid 2) | `DeviceWatcher` Updated | n/a | UNVERIFIED | Later |
| Airplane mode | All `Radio` objects | `Radio.StateChanged` | Turn every `Radio` off/on. This is not the system airplane flag (that is a private radio-manager interface, UNVERIFIED), so Windows' own indicator may disagree | Workaround Public | Later (Wi-Fi + BT tiles cover it) |
| Mobile hotspot | `NetworkOperatorTetheringManager.TetheringOperationalState`, `GetTetheringCapabilityFromConnectionProfile` | No event documented (VERIFIED the class page lists none): read when the panel opens | `StartTetheringAsync` / `StopTetheringAsync`; Learn lists capability `wiFiControl` ([class page](https://learn.microsoft.com/en-us/uwp/api/windows.networking.networkoperators.networkoperatortetheringmanager)); whether an unpackaged exe is refused: UNVERIFIED | Public | Later |
| Brightness, internal panel | WMI `root\wmi:WmiMonitorBrightness` | WMI event `WmiMonitorBrightnessEvent` (VERIFIED `S:.../monitors/brightness/application.rs:16,103`) | WMI method `WmiMonitorBrightnessMethods.WmiSetBrightness` (VERIFIED `application.rs:255-264`) | Public | **MVP** |
| Brightness, external (DDC/CI) | `dxva2!GetMonitorBrightness` on handles from `GetPhysicalMonitorsFromHMONITOR` (KNOWN; Seelen wraps it in `S:src/background/windows_api/monitor/brightness.rs`) | **None.** Seelen polls every 5 s (`application.rs:33-36`). We read once when the panel opens and after `WM_DISPLAYCHANGE`, never poll | `SetMonitorBrightness` (slow; coalesce writes while dragging) | Public | **MVP**; hide the slider when the monitor reports no DDC/CI |
| Night light | Registry `HKCU\Software\Microsoft\Windows\CurrentVersion\CloudStore\Store\DefaultAccount\Current\default$windows.data.bluelightreduction.bluelightreductionstate\windows.data.bluelightreduction.bluelightreductionstate`, value `Data` (Bond CompactBinary blob), settings blob in the sibling `...bluelightreduction.settings` key (VERIFIED `S:src/background/modules/system_settings/nightlight/manager.rs:17-21`) | `RegNotifyChangeKeyValue` on that key | Rewrite the state blob. Format can change between builds | Undoc | Later |
| Focus / Do Not Disturb | `ToastNotificationManagerForUser.NotificationMode` (Win11; VERIFIED `S:.../notifications/application.rs:444-449`) | `NotificationModeChanged` event (VERIFIED, `mode_changed_token`, `application.rs:476-490`) | `IQuietHoursSettings` (CLSID f53321fa-34f8-4b7f-b9a3-361877cb94cf, IID 6bff4732-81ec-4ffb-ae67-b6c1bc29631f) with profile strings `Microsoft.QuietHoursProfile.{Unrestricted,PriorityOnly,AlarmsOnly}` (VERIFIED `S:src/background/windows_api/undocumented/quiet_hours_settings.rs:1-25`; Seelen notes there is no public setter). On failure Seelen opens `ms-settings:notifications` (`application.rs:466-475`). There is no "for 1 hour": we would run our own timer. Seelen does not use WNF | Read/notify Public; set Undoc | **MVP** on/off; timed durations Later |
| Now Playing | `GlobalSystemMediaTransportControlsSessionManager.GetCurrentSession`, `TryGetMediaPropertiesAsync`, `GetPlaybackInfo`, `GetTimelineProperties` | `CurrentSessionChanged`, `MediaPropertiesChanged`, `PlaybackInfoChanged`, `TimelinePropertiesChanged` (VERIFIED imports `S:.../media/players/application.rs:7-11`) | `TryPlayAsync`, `TryPauseAsync`, `TrySkipNextAsync`, `TrySkipPreviousAsync`, `TryChangePlaybackPositionAsync` | Public; works unpackaged (Seelen ships unpackaged) | **MVP** |
| Notifications | See 3.2 | See 3.2 | `RemoveNotification`, `ClearNotifications` | Mixed | Later |
| Screen cast / projection | Projection mode: `QueryDisplayConfig` | `WM_DISPLAYCHANGE` | `SetDisplayConfig` (extend / duplicate / one screen); wireless display connect has no API we know of: launch `ms-settings-connectabledevices:devicediscovery` (the Win+K flyout) (UNVERIFIED on 26300); `Windows.Media.Casting.CastingDevicePicker` is a documented picker for UWP casting, UNVERIFIED for Miracast from an unpackaged exe | Projection Public; Miracast UNVERIFIED | Later: tile with Extend/Duplicate + "Connect to wireless display" |
| Keyboard backlight | No Windows API. `Windows.Devices.Lights.LampArray` only covers HID LampArray devices; whether the Slim 7i keyboard is one is UNVERIFIED. Lenovo Vantage uses a private interface, UNVERIFIED | none | none | Impossible for now | **Skip** |
| Tray icons | See 3.3 | WM_COPYDATA stream via hook | Send the icon's callback message to its owner window | Undoc | CC3 (roadmap M3) |
| Dark mode / accent | `UISettings.GetColorValue`, `AppsUseLightTheme` | `UISettings.ColorValuesChanged` (VERIFIED `S:.../system_settings/application.rs:204`) | Registry + `WM_SETTINGCHANGE "ImmersiveColorSet"` broadcast | Public | Later (Display detail view) |
| VPN, AirDrop/Nearby Share, Stage Manager | Nothing stable | | | Impossible / out of scope | **Skip** |

### 3.2 Notifications and package identity

| Question | Answer |
|---|---|
| Can an unpackaged exe use `UserNotificationListener`? | The Learn page ([link](https://learn.microsoft.com/en-us/uwp/api/windows.ui.notifications.management.usernotificationlistener)) says the user must grant access through `RequestAccessAsync` from a UI thread and lists a `NotificationChanged` event; it does not state a package-identity requirement. Seelen's code comment says the event **only works on MSIX/APPX/UWP builds** (`S:src/background/modules/notifications/application.rs:130`). Treat the live event as unavailable unpackaged; whether `GetNotificationsAsync` works unpackaged on 26300 is UNVERIFIED (20-line test) |
| What is package identity? | The process has a package family name: an MSIX install, or a **sparse package** (MSIX manifest registered with an external location so the exe stays Win32). It unlocks the listener capability and toast activation. It needs a signing certificate, so it lands with M6 |
| Unpackaged workaround Seelen uses | Read `%LOCALAPPDATA%\Microsoft\Windows\Notifications\wpndatabase.db` (SQLite, WAL mode) and watch `wpndatabase.db` / `-wal` for file changes (`S:src/background/modules/notifications/wpn_service.rs:1-30`). Undocumented, schema stable since Win10 1607 per Seelen; event-driven, not polling; costs an SQLite dependency |
| Replace Windows' banners? | Seelen hides the ShellExperienceHost windows with a WinEvent hook (`S:src/background/widgets/weg/hook.rs`). Recommendation: do not in v1 |

### 3.3 Tray icons: the Seelen approach

| Step | What Seelen does | Where |
|---|---|---|
| 1 | Find `Shell_TrayWnd`, install `SetWindowsHookExW(WH_CALLWNDPROC)` using a DLL loaded into explorer's tray thread | `S:src/background/modules/system_tray/application/tray_hook_loader.rs:50-56` |
| 2 | The hook sees the `WM_COPYDATA` (`dwData == 1`) that `Shell_NotifyIcon` sends; copies a 32-bit-handle `NOTIFYICONDATA` layout (bounds-checked) and forwards it over IPC | `S:src/hook_dll/src/lib.rs:43-215` |
| 3 | Overlay badges: same hook catches `WM_USER+79` (icon) and `+85` (description) on `MSTaskSwWClass` | `lib.rs:29-38` |
| 4 | Icons registered before start: broadcast `TaskbarCreated` so apps re-add | `S:.../system_tray/application/util.rs` |
| 5 | Clicks: post the icon's callback message to its owner HWND | `S:.../system_tray/application/tray_icon.rs` |

Risks: the DLL runs in explorer (a bug can crash it; the guardian restores the desktop but the user sees a flash); must be x64; the `NOTIFYICONDATA` wire layout is OS-specific (re-test per Windows build); hidden icons (`NIS_HIDDEN`) must be honoured; Smart App Control likely blocks an unsigned DLL injected into explorer (UNVERIFIED), so signing matters here. It is read-only: Windows' own tray keeps living in the hidden taskbar, so apps never notice if the hook dies.

## 4. Visual and motion spec (Concept A)

Material matches the dock: blur 40, tint `#16161A` @ 50%, 2.5% grain, 1 px vertical-gradient rim, radius 20 at panel scale. Sizes are logical px.

### 4.1 Menu bar

| Item | Spec |
|---|---|
| Height | 24 (also the appbar reservation); glass, radius 0, rim on the bottom edge only |
| Spacing | 12 side padding; items 28 wide x 24 tall hit box, 4 gap |
| Left | Focused app name: Segoe UI Variable Text, Semibold 13/16, `#F5F5F7` @ 90% |
| Right (right to left) | Clock, Control Center icon, battery, Wi-Fi, optional sound/BT, tray icons |
| Icons | 16x16 glyph in Segoe Fluent Icons for built-ins; tray icons downscaled from the app icon to 16x16 (monochrome option off by default) |
| Clock | "Fri 17 Oct  9:41", Segoe UI Variable Text 13 regular, tabular figures |
| Hover | Rounded highlight radius 6, white @ 10%, 80 ms |

### 4.2 Control Center panel

| Item | Spec |
|---|---|
| Size | 336 wide, auto height (about 440 collapsed), max = work area height - 16 |
| Anchor | Right edge aligned to the icon, 6 below the bar, kept inside the screen; opens on the monitor whose bar was clicked |
| Grid | 12 outer padding, 4 columns, 12 gap: column = (336 - 24 - 36) / 4 = 69; square tile 69x69, wide tile 150 (2 cols) |
| Radius | Panel 20 (dock token); tiles 10 (concentric 20 - 12 = 8, rounded up for optical balance) |
| Tile fill | white @ 8% on glass, pressed 14%; "on" state uses the Windows accent (`UISettings`) |
| Type | Segoe UI Variable Text. Title 13/16 Semibold; subtitle 11/14 regular @ 60%; minimum size 11 |
| Toggle tile | 32 px circle with 18 px glyph, label 12/16 below, state line 11/14. Click toggles; a chevron appears top-right on hover (and the label is also a hit target) to expand |
| Slider | Full width, 36 tall pill, radius 18; fill white @ 90%; glyph inside left; 1:1 drag, wheel step 2%, arrow keys 5% |
| Now Playing | Wide tile, art 56 (radius 10), title 13 semibold, artist 11 @ 60%, 3 transport buttons 28 |
| Detail view | Replaces the grid: back chevron + title, then 40-tall rows (icon, label, checkmark/lock/signal/battery). Wi-Fi: signal, SSID, lock. BT: device glyph, name, battery %. Back chevron or Esc returns |

### 4.3 Motion (composition animations, not UI-thread)

| Transition | Spec |
|---|---|
| Panel open | Opacity 0 to 1 in 120 ms; scale 0.96 to 1 about the anchor with a spring (damping ratio 0.85, response 0.25 s); translate Y -6 to 0 |
| Panel close | 100 ms ease-in fade, scale to 0.98, no spring |
| Detail view | Grid fades out 100 ms, detail fades in 160 ms sliding 12 px from the right; window height follows a critically damped spring (response 0.3 s) |
| Toggle | Fill cross-fade 150 ms; glyph scale 1 to 0.92 to 1 on press |
| Notification sheet | Slides from the right, 360 wide, spring response 0.3 s |
| Reduce motion | If `UISettings.AnimationsEnabled` is false, replace springs with 80 ms fades |

### 4.4 Keyboard and accessibility

| Area | Spec |
|---|---|
| Open | Configurable hotkey (default suggestion `Ctrl+Alt+C`); the bar icon is also a button. Win+A belongs to Windows' Quick Settings |
| Navigation | Tab/Shift+Tab across tiles; arrows inside grid and lists; Space/Enter activates; Right expands; Left/Esc collapses, then closes; 2 px accent focus ring |
| Screen reader | No XAML controls, so a UI Automation provider per tile (`Name`, Toggle pattern, RangeValue for sliders). Effort UNVERIFIED; its own line in CC4 |
| Contrast | Text >= 4.5:1 on glass over a white wallpaper; test it |
| High contrast | Opaque `GetSysColor` palette instead of glass |
| DPI | Re-layout on `WM_DPICHANGED` |

## 5. Architecture sketch

```
guardian.exe     unchanged role: owns "taskbar hidden". Also releases the bar's appbar
                 reservation (ABM_REMOVE) if halo.exe dies. No new modules.
halo.exe         thread dock (existing)
                 thread bar      per-monitor bar windows (own Compositor + DispatcherQueue)
                 thread services event sources only, no UI; posts small "dirty" messages
                 thread panel    created on first open, torn down ~60 s after close
trayhook.dll     optional, in explorer's tray thread only (CC3)
```

| Question | Decision |
|---|---|
| Same process as the dock? | Yes, separate threads. A second process would add a second ~ 17 MB baseline for little isolation, since the guardian already restarts halo.exe on a crash. Split out the bar only if it proves unstable |
| Event sources | All on the services thread: Core Audio callbacks, WinRT Radio / WiFiAdapter / NetworkInformation / media-session events, `PowerSettingRegisterNotification`, `NotificationModeChanged`, `RegNotifyChangeKeyValue` (night light). Each posts "item X changed" to the bar thread, which re-reads that one value. The only timer is the minute clock |
| Lazy init | A service starts when a bar item or the panel first needs it. Wi-Fi `ScanAsync` runs only while the Wi-Fi detail view is open. DDC/CI is read only on panel open |
| Guardian | Adds one duty: the appbar reservation. The tray hook is deliberately not in the guardian |
| Failure isolation | Each service has try/catch and a "module unavailable" state (greyed tile linking to Settings); a service that throws is disabled for the session instead of retried in a loop. Hook DLL: bounds-checked, no allocation in the hook procedure beyond a fixed buffer, exceptions swallowed |
| Memory | Bar + services: <= +6 MB private over the dock's 17.5 MB. Panel open: <= +20 MB, freed within 60 s of closing |
| CPU | 0 ms over 60 s idle with panel closed and with it open (same method as `tools/smoke.ps1`); slider drag <= 5% of one core |

## 6. Phased plan

| Phase | Scope | Done when (measured) |
|---|---|---|
| **CC0 spike** (2 days) | Bar window per monitor with glass material, appbar reservation, clock, app name; guardian releases the reservation | Work area shrinks by exactly 24 logical px and returns after `taskkill`; 0 ms CPU over 60 s idle with the bar; RAM delta recorded |
| **CC1 MVP panel** (about 1 week) | Volume + output picker, Mic mute, Brightness (internal + DDC/CI), Wi-Fi, Bluetooth, Do Not Disturb, Now Playing, battery + power mode; spring open/close; keyboard nav | Each tile's API test recorded on build 26300; a system-side change shows in the UI within 500 ms with zero timers; 0 ms idle CPU over 60 s with the panel open; open to first frame <= 100 ms warm (<= 250 ms cold); no frame > 20 ms during the open spring |
| **CC2 Notification Center** | Clock click: month calendar, then notification list from `wpndatabase.db` (or `UserNotificationListener` once a sparse package exists) | A new toast shows in the list <= 1 s after Windows shows it; clear works; tested with 3 apps |
| **CC3 tray** (= roadmap M3) | `trayhook.dll`, status items with click/right-click forwarding, overflow chevron, badges | Discord, Steam, OneDrive, Teams match Windows' tray for a week; explorer survives 100 hook load/unload cycles |
| **CC4 extras** | Per-app volume, Night light, hotspot, airplane via radios, Focus durations, cast tile, UI Automation providers | Each API check passes; Narrator reads every control |
| **CC5 polish** | Pin/unpin tiles to the bar, reorder, bar auto-hide, layout edit | Config survives live reload |

## 7. Open questions for Ziyaad

| # | Decision | Options | Recommendation |
|---|---|---|---|
| 1 | Top bar: reserve a strip or float? | (a) 24 px reserved on every monitor, (b) floating overlay (covers caption buttons), (c) reserved on the primary only | (a); (c) if the 1080p monitor feels cramped |
| 2 | What does the bar's left side show? | (a) focused app name only, (b) app name + that window's min/close buttons, (c) nothing | (a); (b) later and optional |
| 3 | Notifications: keep Windows' banners or replace them? | (a) keep Windows' banners, add a list to the sheet later, (b) hide Windows' banners and draw ours | (a) until a signed sparse package exists |
| 4 | Tray hook (code in explorer): when on? | (a) from the first usable build, (b) off by default behind a setting until it has run a week clean | (b), per DESIGN.md reliability rules |
| 5 | Code signing / sparse package | (a) pay for a certificate now, (b) defer to M6 | (b), unless Smart App Control blocks your own builds sooner |
| 6 | Do Not Disturb via the undocumented `IQuietHoursSettings` | (a) use it and fall back to opening Settings, (b) read-only tile that opens Settings | (a) |
| 7 | Control Center hotkey | (a) `Win+A` (Windows' Quick Settings owns it), (b) `Ctrl+Alt+C`-style, (c) none | (b); no Win-key hooks before M4 |
| 8 | Which glyphs live in the bar next to the CC icon? | (a) battery + Wi-Fi, (b) icon only, (c) everything (sound, BT, Focus) | (a), user-configurable later |
