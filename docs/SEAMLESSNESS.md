# Halo seamlessness work — 21 October 2026

The dock is the reference for quality. Preserve its behavior and visual system. Start works for the founder; its next pass is visual hierarchy and layout, preserving search, recent apps/files, and account actions. Control Center needs both functional and visual work.

## Implemented in this batch

- Wi-Fi, Bluetooth, and audio output selection have separate detail pages with Back/Escape, bounded rows, wheel input confined to the list, Up/Down scrolling, and a visible shell-action row. Device counts are no longer silently capped at six. Data updates cancel a pending pointer press so a reordered row cannot select a different device.
- Compact home uses the existing four-pixel spacing scale. Its header preserves enough room for the date; horizontal toggle gaps stay consistent. Compact power choices use the media card's space temporarily.
- Wi-Fi reads actual SSIDs from connected WLAN profiles independently of the default Internet route. Nearby discovery runs on explicit expansion/Refresh, off the UI thread, with a bounded wait for native scan completion. Rows preserve adapter and saved-profile identity, connected state, security, and signal strength. New networks open Windows' credential/setup flow. Accepted connection requests remain pending until OS readback confirms the selected adapter/profile; timeout and immediate errors are visible.
- Bluetooth uses a fresh DeviceWatcher session per open/refresh, ID-keyed updates, connected-state events, locked value snapshots, and cancellation/session guards. It displays paired devices; pairing and device connection actions remain in Windows Settings.
- Discovery presents loading, connecting, empty, unavailable, permission-denied, partial-result, and failure states, with Refresh/Retry/Settings routes. It does not change permissions itself.

## Visual directions for review

- [Start preview](previews/start-refresh.html): 720 × 560 logical-pixel roomy proposal; 600 × 500 compact; light/dark; local example search. Native functionality is unchanged until mock approval. App symbols in the mock are placeholders; production retains actual app icons.
- [Control Center preview](previews/control-center-refresh.html): quieter grouped connections, substantial display/sound controls, compact secondary toggles/media, bounded details, stable footer. This is a design proposal, not a screenshot of the native build. The native detail-page architecture is implemented; the full proposed home styling remains a subsequent pass.

## Evidence and limits

Run from the Halo repository:

```powershell
python tools/control_center_layout_test.py
python tools/wifi_ssid_test.py
```

The first fixture compiles actual production Layout() with synthetic monitor/configuration data. It covers the six target height/scale combinations, network/power states, and reachability across 20 audio/24 Wi-Fi/24 Bluetooth rows. It checks geometry, not rendered composition, input dispatch, or hardware.

The second fixture extracts actual ConnectedSsid() and provides deterministic fake WinRT profiles, including offline Wi-Fi alongside VPN/Ethernet and failed/empty adapters. `tools/radio_policy_test.cpp` tests discovery error classification, signal quality bounds, and profile presence without accessing hardware. Isolated build/logs live under `build/seamless-2026-10-21/`.

Preview HTML is rendered separately in headless Chromium. Native appearance, real radio/watcher timing, permissions, mixed-monitor DPI transitions, input focus, accessibility, and repeated open/close performance remain **UNVERIFIED**. The live login shell is not replaced by this work. Installation and publishing belong to the CEO.

## Remaining work in order

1. Review the Start mock before its native layout changes. Validate this separate Control Center build in an isolated desktop/account; complete real radio and mixed-DPI checks before installing it.
2. Apply the approved visual direction to native home surfaces, with light/dark screenshots and keyboard focus states. Keep all existing controls and actual app icons.
3. Complete keyboard focus traversal and slider operation, focus return, accessibility, notification activation preservation/error feedback, and truthful hotspot completion. These remain open findings in the earlier bug audit.
4. Capture animation/input and memory baselines; verify repeated open/close, sleep/resume, display changes, and device hotplug. Do not infer smoothness from a successful compile or screenshot.

## Research basis

Microsoft documents that native Wi-Fi scanning and current-connection queries can be denied without location consent, recommends GetConnectedSsid for SSID-only access, and calls for a Settings route on denial: [Wi-Fi access and location](https://learn.microsoft.com/en-us/windows/win32/nativewifi/wi-fi-access-location-changes). Scanning remains consent-bound.

DeviceWatcher supports dynamic additions/updates/removals. Stop is asynchronous; a stopped watcher cannot simply be restarted while it is still stopping. Halo uses fresh sessions and unsubscribes old callbacks: [DeviceWatcher lifecycle](https://learn.microsoft.com/en-us/uwp/api/windows.devices.enumeration.devicewatcher).

The visual recommendations emphasize clear toggle labels/state, predictable placement, readable hierarchy, and connected transitions. Specific preview dimensions are design recommendations, not platform requirements: [Apple toggles](https://developer.apple.com/design/human-interface-guidelines/toggles), [Microsoft motion](https://learn.microsoft.com/en-us/windows/apps/design/motion/), [Microsoft typography](https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/typography).
