# Halo 2.0 resource review

Base: 2a56f42. Changes are limited to requested media/notification fixes and resource reductions.

Media capture regression: synchronous WM_CAPTURECHANGED consumed the pending command inside ReleaseCapture. The actual-handler fixture failed twice before the fix and passes after copying the command before releasing capture. Dock tests cover active/paused visibility, group centering and media targets. Real-player playback remains UNVERIFIED.

Notifications now use compact grouped alerts above a calendar widget, plain headings, restrained borders and shared spacing/radius tokens. Reference: [Apple Notification Center](https://support.apple.com/guide/mac-help/get-notifications-mchl2fb1258f/mac). The isolated synthetic preview runs without real notifications. Native tool-window discovery did not expose it for screenshots; aesthetic and mixed-DPI approval remain UNVERIFIED.

Resource reductions:

- One hit-test per pointer event rather than up to four.
- Skip unchanged media input regions and dock overlap rectangles.
- Release detached preview/menu visual graphs after closing.
- Avoid restarting label animations at identical destinations.
- One process snapshot per Preferences sample instead of three.
- Cache notification shadows as small textures; plain heading/empty rows allocate no glass/shadow/rim.

Reviewed shell/dock, tracking/icons/pins, hooks/taskbar/guardian, monitors/intellihide/swoop, panels, services/sampling, previews/menu/labels, settings, motion/effects/theme and headers. Two read-only reviewers covered independent groups; the primary agent verified applied findings in source. Deferred cache eviction, brightness worker changes, sampler buffer reuse and expression redesign to preserve behavior and avoid unmeasured complexity.

Installed-shell baseline: 0.0 CPU seconds over five idle seconds, 49,410,048 private bytes, 112,877,568 working-set bytes, 40 threads. This is not a controlled before/after comparison. Percentage RAM/CPU/GPU savings are UNVERIFIED; removed work and retained handles are confirmed in code.

Checks: media capture 0 failures; dock both modes pass; Start/sampler pass; search 10/0; Settings launch 0 failures; Control Center layout 0 reachability failures. Separate output: `build/halo-macos-efficiency-2026-10-21.exe`. Preferences is also rebuilt. No installation performed.
