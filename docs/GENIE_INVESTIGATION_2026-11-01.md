# Genie investigation — 2026-11-01

Halo's `mods/halo-genie.wh.cpp` is a fork of MacOS Minimize Animation, but its current DirectComposition renderer uniformly scales a snapshot; it does not implement a genie mesh. Maximize/unmaximize use separate zoom tracks. `src/swoop.cpp` is a DWM thumbnail scale fallback for processes the mod cannot reach, and restore through this path is triggered by a dock click. These are distinct paths and require separate verification.

References inspected:
- https://github.com/ramensoftware/windhawk-mods/blob/main/mods/macos-minimize-animation.wh.cpp — upstream project, with modern mesh renderer and window lifecycle handling.
- https://github.com/ramensoftware/windhawk-mods/blob/main/mods/genie-minimize-animation.wh.cpp — alternative implementation.
- https://windhawksoftware.com/mods/macos-minimize-animation/ — describes modern renderer and capture limitations.

Next: compare the upstream renderer and its license at a pinned revision, prototype integration retaining Halo's dock icon protocol and maximize path, and test capture, failure cleanup, restore handoff and multiple displays in a separate harness. Evaluate a fallback for applications outside Windhawk injection. Native smoothness, actual failure cause, build and integration remain UNVERIFIED. Do not replace the running shell.

## Prototype, 2026-11-01
Implemented 32 DirectComposition strips driven by the upstream MIT lamp curve in `mods/halo-genie.wh.cpp`, preserving Halo's icon protocol and existing maximize zoom. The compositor gets the animation timelines once; this is a strip approximation, not the upstream Direct2D mesh. Separate test window smoke output: `genie kind=0 ok=1 dur=320ms waited=375.2ms`, `genie kind=1 ok=1 dur=320ms waited=369.5ms`, `lamp smoke failures: 0`. Geometry test: `lamp geometry failures: 0` for targets above and below. This does not measure frame timing, actual hooked applications, visual seams, mixed DPI, or maximize behavior. The non-injected DWM fallback still scales; full fix remains incomplete. No installation or shell restart.

## 2026-11-01 smoothing and install
Increased lamp strips from 32 to 48, extended duration to 380 ms, and reduced curve solving iterations. The test window reported timeline setup 12.7/15.1 ms for minimize/restore; geometry failures 0 and smoke failures 0. These measurements do not establish visual smoothness or real-application behavior. Windhawk installer compiled and selected `local@halo-genie_2.2.1_648494.dll` in its configuration. Previous ini backed up to `build/halo-genie-before-2026-11-01.ini`; previous DLL remains. Halo was not restarted. Windhawk process was not found in the process-name check; engine loading and live effect UNVERIFIED. The fallback remains a scale effect.

## 2026-11-01 activation follow-up
Windhawk was not running even though its configuration selected the mod. Started the portable Windhawk tray engine and configured its HKCU Run entry (`windhawk.exe -tray-only`). Existing other Windhawk mods were disabled. A separate process confirmed `genie mod loaded in test process: yes` and, after a real ShowWindow minimize, `genie ghost after minimize: yes` (exit 0). Explorer and ApplicationFrameHost each showed the 2.2.1 DLL loaded. This establishes activation and a ghost window, not visual quality or coverage in all apps. The separate non-injected Halo fallback still scales; browser coverage and user's native assessment remain UNVERIFIED. Halo and guardian were not restarted.

## Native Halo prototype, 2026-11-01
Windhawk was gracefully stopped at CEO request, and its HKCU Run entry removed; Halo/guardian were not restarted. Halo's `Swoop` now constructs a lamp from 32 cropped DWM thumbnails of the tracked window, without process injection, and uses the MIT lamp curve credited above. The cross-process fixture showed a minimize overlay, dock-restore callback and overlay cleanup, with reduced motion suppressing the effect; geometry tests report zero failures. The test does not validate thumbnail pixels, browser/elevated app coverage, visible seams, frame timing, or native UX. Restore initiated outside Halo's dock is not intercepted. The separate build also contains unrelated pre-existing working tree changes. The running Halo instance has not been replaced; the prototype is not active until the CEO installs a reviewed build.
