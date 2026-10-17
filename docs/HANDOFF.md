# Halo handoff (2026-10-17)

Read this first, then `README.md`, `docs/DESIGN.md` and `docs/CONTROL-CENTER.md`.

Halo is a macOS-style dock that replaces the Windows 11 taskbar. It is written in
C++/WinRT on Windows.UI.Composition: expression animations drive the magnification
and keyframes drive the bounce and presence animations. The repo is private at
github.com/ziyaad-mallick/halo. `main` is at `f91b2d5`.

## Ground rules from the owner

- **Visual rules:**
  - Stock Windows icons only, with no masks and no icon packs.
  - Sizing must stay identical to Quiet Dock (numbers below).
  - Animations must look polished: a spring reveal, a bounce on click, and the edge cases handled.
- **Design questions go to him.** Don't guess on look or behaviour.
- **Don't touch the Windhawk UI settings.**
- **Don't add autostart or other persistence yourself.** He runs `build/guardian.exe --autostart on` himself if he wants it.
- **Test without disrupting his live session.** Don't launch apps on a timer. An earlier test opened and closed MS Paint every 15 s and he noticed.

## Build and run

These steps must run in Git Bash. In PowerShell, `bash` isn't on PATH.

```sh
tools/build.sh all            # or: halo | guardian | icons_test
build/guardian.exe            # ON: dock runs, taskbar hidden
build/guardian.exe --off      # OFF: taskbar back (also --restore; --quit exits)
```

- **The compiler** is the LLVM-MinGW clang that ships with Windhawk. You don't need the Windows SDK.
- **Close the live copy before relinking.** The running guardian/halo locks `build/*.exe`, so the linker fails with "Permission denied" while they run.
  - Send WM_CLOSE to the `HaloDockWindow` window, or run `build/guardian.exe --quit`.
  - Then rebuild and start it again.
- **Smart App Control is ON** and sometimes blocks a freshly built unsigned exe ("Application Control policy has blocked this file").
  - A trivial source change plus a rebuild usually produces a binary that is allowed.
  - The real fix is code signing (Azure Trusted Signing, or SignPath for OSS). This is undecided.
- **Logs:** `%LOCALAPPDATA%\Halo\guardian.log`.
- **Test hooks:**
  - `HALO_TEST=1` enables `WM_APP+2/+3`.
  - `HALO_TEST_WINKEY=1` makes injected Win presses count.
- **Tools:**
  - `tools/smoke.ps1`: memory and CPU.
  - `tools/m0-checks.ps1`: multi-taskbar check, reveal burst, `-ExplorerRestart`.
  - `tools/shots.ps1`: screenshots of the rest/hover/open/close/press/bounce scenarios.
  - `tools/clickthrough.ps1`.
  - `tools/icons-sheet.ps1`.

## Code map

| File | What it does |
|---|---|
| `src/guardian.cpp` | Always-on ON/OFF switch, described below |
| `src/halo.cpp` | Main window and message loop; `Activate`, `RefreshDock`, Win-key handling, genie dock-icon query (`DockIconForWindow`, holds the dock 700 ms) |
| `src/dock.h/.cpp` | Keyed `SetItems` with presence animation, `SetPressed`, `Bounce`/`Settle`, `HitTest`, `RestWidth`/`MaxWidth`, `RestIconCenter(i)`; monitor-wide window with a `SetWindowRgn` input region |
| `src/apps.cpp` | Window tracking, described below |
| `src/icons.cpp` | Pins and icons, described below |
| `src/winkey.cpp` | Win-key tap, described below |
| `src/taskbar.cpp` | Hides and restores `Shell_TrayWnd` (all monitors) |
| `mods/halo-genie.wh.cpp` | Windhawk genie mod; `tools/install-genie.py [--include * --off --log]` installs it into portable Windhawk (`~/Windhawk`) |

**`src/guardian.cpp`**
- **Toggle:** Win+Alt+T. `RegisterHotKey` returns error 1409 on this machine, so the guardian falls back to a WH_KEYBOARD_LL hook that runs on its own thread.
- **Second-instance IPC:** the registered message `HaloGuardian.Command`.
- **Crash loop:** 3 crashes in 2 minutes turns Halo OFF.
- **Window:** a hidden WS_POPUP tool window. Don't use HWND_MESSAGE here: message-only windows never receive the TaskbarCreated broadcast, so the taskbar came back after an explorer restart.

**`src/apps.cpp`**
- Window tracking uses SetWinEventHook, out-of-context.
- **App key:** the AUMID, or else the lower-cased exe path.
  - Packaged desktop apps get their AUMID from `GetApplicationUserModelId(process)`. Without it, running apps showed up twice.
  - Explorer windows are mapped to `Microsoft.Windows.Explorer`.

**`src/icons.cpp`**
- **Pins** come from the Taskband `Favorites` registry blob, in visual order. Store apps appear there as `shell:AppsFolder\<AUMID>`.
- **Store icons** use the unplated `Square44x44Logo` from `AppxManifest.xml`, decoded with WIC.

**`src/winkey.cpp`**
- Tapping Win alone reveals the dock and sends Win+Alt+Space, which opens PowerToys Command Palette.
- It is an LL hook on its own thread. It swallows the Win release and re-injects VK 0xE8 down/up followed by Win up.
- Its own injected input carries the marker `0x48414C4F` in dwExtraInfo.

**Geometry** (logical px, taken from Quiet Dock):
- **Icons:** 28 at rest, magnified ×1.57 to 44 at the peak, with a cosine falloff over a 100 px radius.
- **Spacing:** gap 10, padding 10.
- **Pill:** 50 high, radius 20, floating 6 px above the bottom of the screen.
- **Glass:**
  - Blur 40, tint #16161A at 50%, 2.5% grain.
  - 1 px gradient rim: #70FFFFFF at 0, #18 at .35, #0A at .7, #26 at 1.
- **Running dot:** 4 px, white at 70%.

**Genie protocol:**
- The mod sends the registered message `Halo.DockIconForWindow` with wParam = HWND and lParam = 1 (minimize), 2 (restore) or 3 (query).
- The reply packs:
  - bits 0-15: icon size
  - bits 16-31: x as int16
  - bits 32-47: y as int16
  - 0 means unknown.

## Done and verified

- **M0 spike:**
  - The spring reveal overshoots about 4 px.
  - Reveal latency is 80–100 ms.
  - After an explorer restart the taskbar stays hidden (TaskbarCreated is handled).
- **Dock polish:**
  - Stock icons, including Store apps, all at the correct sizes.
  - Presence animations for opening and closing apps.
  - Press state.
  - A macOS-style bounce on launch. It runs only if the app wasn't already running, and it finishes its cycle instead of being cut off.
- **Tray/clock pill removed** at the owner's request.
- **Running apps no longer duplicated** (Claude, Terminal, Spotify); 18 icons are correct.
- **Guardian switch:** the Win+Alt+T toggle works both ways and leaves no stuck modifiers. The `--off`, `--quit` and `--autostart` flags work.
- **Win key alone** opens Command Palette and reveals the dock.
- **Genie v1** works end to end, for minimize and restore. It is installed live with `Include=*`.

## Owner's complaints, and their status

| Complaint | Status |
|---|---|
| Genie isn't smooth | **OPEN.** v1 renders on the CPU (D2D DC render target + UpdateLayeredWindow, timer-paced). The fix is task 1. |
| Can't see an app's multiple windows on hover | **OPEN.** Task 3. |
| No bobbing when clicking an icon | Fixed in `7f95d16`. `Settle` was killing it within 120 ms, and ShellExecute blocked before `Bounce` ran. |
| Tray/clock pill looked bad | Removed. A status cluster and Control Center replace it (tasks 2 and 4). |
| Duplicate running icons | Fixed in `494c216`. |
| Taskbar came back after explorer restart | Fixed in `d6f1402`. |
| MS Paint opening every 15 s | It was a test probe, not a bug. Stopped. |
| Smart App Control blocks unsigned builds | **OPEN.** Needs code signing. Undecided. |
| Memory 36.5 MB private (README says 17.5) | **OPEN, uninvestigated.** About 30.5 MB came before the polish work (M1's ~27 icons plus Store icon loading); about 6 MB came from the polish. |

## Next, in priority order

1. **Smooth GPU genie.**
   - **Plan:** the mod only captures the window and hands the pixels over through shared memory. Halo renders a strip-based genie on the GPU with Composition keyframes.
   - **Existing work:**
     - WIP on branch `worktree-agent-a8192aa49071b9b8d`, commit `edb3933`: `src/genie.h`, the handoff protocol and a frame-capture benchmark. It was paused mid-edit and **may not build**.
     - A bench script is on `worktree-agent-a99d4293e409462d0`, commit `c69d20f`.
   - **When it works:** add the same swoop for maximize/restore.
2. **Dock left cluster.** Order from the far left: Windows Start button, then status icons, then the time.
   - **Start button:** the stock Win11 logo. Clicking it opens Start by injecting Ctrl+Esc, marked so the winkey hook ignores it.
   - **Status icons:** a Wi-Fi glyph that shows whether it's connected, and battery % with the charging state.
   - **Time:** at the extreme left.
   - **Fonts:** Segoe Fluent Icons and Segoe UI Variable.
   - **Updates:** event-driven, with a minute-aligned clock timer and no polling.
3. **Hover window previews** (DwmRegisterThumbnail) for apps with several windows. Also **front-click minimize**: clicking the icon of the app that's already in front minimizes it. Today Activate doesn't do this.
4. **Control Center MVP.**
   - **The panel:** a floating glass panel at the top-right that auto-hides.
   - **How to open it:** Win+A, taken over while Halo is ON, or by moving the mouse into a top-right hot zone.
   - **Modules:**
     - clock, battery and power mode
     - volume and output device
     - mic
     - brightness
     - Wi-Fi and Bluetooth
     - Do Not Disturb (DND)
     - Now Playing
   - **Tray apps** are listed in the panel. Read them through UIA on the hidden taskbar's tray rather than a hook ("tray hook off until proven").
   - `docs/CONTROL-CENTER.md` is the research. Its top-bar recommendation is **superseded** by the decisions above.
5. **Later decisions** (already made, not built yet):
   - **Pins:** Halo's own pin list, with drag to reorder, a "poof" on drag-off, and "Keep in Dock".
   - **Intellihide:** hide only when a window covers the dock.
   - **Monitors:** the dock follows the cursor across monitors.
   - **Labels:** app names on hover.
   - **Recent apps:** a Recent apps section.
   - **Right-click:** a glass menu with the jump list.
   - **Search:** left for now. Command Palette covers it.
6. **Housekeeping:**
   - Investigate memory.
   - Test with a second monitor once one is plugged in.
   - **License:** MIT is recommended. The owner decides before the repo goes public.

## Worktrees

Agent branches live under `.claude/worktrees/` (untracked). Everything useful is already merged into `main` **except** `a8192aa…` (GPU genie WIP) and `a99d42…` (bench). The rest can be deleted once those two are salvaged.
