# Halo handoff (2026-10-17)

Read this first, then `README.md`, `docs/DESIGN.md` and `docs/CONTROL-CENTER.md`.

Halo is a macOS-style dock that replaces the Windows 11 taskbar. It is written in
C++/WinRT on Windows.UI.Composition: expression animations drive the magnification
and keyframes drive the bounce and presence animations. The repo is private at
github.com/ziyaad-mallick/halo. `main` is at `9b9607a`, 28 commits ahead of
`origin/main` — nothing has been pushed yet.

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
| `src/launcher.h/.cpp` | The Launcher panel above the dock, described below |
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
- While the Launcher panel is up (`WatchEscape(true)`) it also posts `EscapeMessage()` on each Esc press, without swallowing it.

**`src/launcher.cpp`** (the Launcher panel)
- A glass panel above the dock, opened with the Command Palette by the Win tap or the Start logo; a second tap closes both. A top-level window of its own (`HaloLauncherWindow`, no activation) on the dock's compositor, so Command Palette keeps the keyboard.
- Recent apps: an MRU of launch targets in `%LOCALAPPDATA%\Halo\recent.txt`, fed by `WindowTracker::onForeground` and dock launches. Recent files: the newest non-folder `.lnk`s in FOLDERID_Recent. Bottom row: account picture from `C:\Users\Public\AccountPictures\<SID>`, display name, Settings, Power menu.
- Dismissal: a foreground WinEvent hook that exists only while the panel is up (Command Palette seen, then anything else gets the foreground, or 800 ms pass without the palette), Esc, a second Win tap, or launching from the panel. The dock does not auto-hide while it is up.
- Tests: `HALO_TEST=1` makes power actions log only (`HALO_TEST_POWER_DRYRUN=1` does too); `HALO_TEST_RECENT=<file>` swaps the MRU file; WM_APP+8 drives the panel (see `tools/launcher-shots.ps1`).

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

## Work in flight (2026-10-17, later the same day)

Two agent branches are open and **not merged**. `main` itself is untouched apart
from the untracked files listed under Memory below.

### `agent-genie-salvage` — task 1, the smooth GPU genie (11 commits)

Ports the paused WIP onto `main` and debugs it. Full list, oldest first:

- `4e4d72c` Halo plays the animation with Composition from the snapshot the mod hands over
- `7d825a1` Genie mod 2.0: capture once, hand it over via shared memory, wait for Halo's first frame
- `a07880c` tools: framecap, testwin and dbwin for genie debugging
- `8b40446` genie-bench: measure click-to-frame and presented gaps
- `99ddfc2` genie_selftest: play the GPU genie in one process, no mod and no live Halo
- `2cb5247` Genie: rebuildable debug logging behind `HALO_GENIE_DEBUG`
- `afd8f25` framecap: probe every output; the first that renders wins
- `ab7723e` genie_selftest: draw marker rects first and gate on DWM composing
- `5c9da95` expr_probe: bisect expression-throw, batch hold and layered targets
- `009553c` probe2: batch hold versus attach order and visibility
- `66573b9` genie-shots.sh: run the selftest under a framecap recording

Files: 14 changed, +1791/−26. `src/genie.cpp` (+313, new), `src/genie.h` (+65,
new), `mods/halo-genie.wh.cpp` (+222, v2.0.0), `src/interop.h` (+19, the interop
structs move out of `halo.cpp` so there is one definition), `src/halo.cpp`
(+24/−26: `kGenieHoldMs` 700→900, the `Halo.Genie` message and its dispatch, the
genie step in `Create`), `tools/build.sh` (+23, adds `framecap` and `testwin` —
note `all` does not include them), and new tools `framecap.cpp`, `testwin.cpp`,
`expr_probe.cpp`, `probe2.cpp`, `genie_selftest.cpp`, `genie-shots.sh`,
`genie-bench.ps1` and `dbwin.ps1`.

It builds, and part of the plumbing is confirmed by running code: the in-process
handoff works (`.ready` fires ~60–94 ms, the snapshot uploads through
`ICompositionDrawingSurfaceInterop`), malformed expressions really do throw at
`StartAnimation` (what `expr_probe.cpp` was for), and the probe2 batches hold their
full ~530 ms for the keyframe and expression cases.

**The animation still does not play — that is the open bug.** Inside the real
`Genie::Start` the scoped batch completes in **~0–31 ms**, so the completion
callback fires instantly and nothing animates, while probe2's nominally equivalent
cases hold ~531 ms; the distinguishing factor was never identified. `framecap`
recorded **185 frames, all byte-identical**, i.e. no animation was ever captured. So
treat "it compiles" as proving nothing here.

The debug aids are committed deliberately (`HALO_GENIE_DEBUG` logging in
`src/genie.cpp`, plus `expr_probe.cpp`, `probe2.cpp`, `genie-shots.sh`) —
**trim the `Dbg` instrumentation before merging to `main`.**

**Task 2's dock left cluster was not started.** The v2 mod is **not installed**, and
the real end-to-end path (live `halo.exe` plus mod 2.0.0) is untested by design —
installing the mod is the owner's call. To reproduce the isolated runs:
`bash tools/build.sh genie_selftest && build/genie_selftest.exe`.

### `agent-previews` — task 3, hover previews (designed, not implemented)

Branch `agent-previews`, **2 commits**, worktree clean:

- `e6f0430` Thumbtest: probe DWM thumbnail destination styles for the previews
- `f3a0d53` Notes: previews design reasoning and the questions for the owner

`docs/NOTES-previews.md` holds the design. **3a** is designed in full as a
header-only `src/previews.h` module (an agent may not touch `tools/build.sh`): the
choreography, hide paths, geometry anchoring and leak-safe register/unregister.
**3b** sketches an `Activate` rewrite that keeps the launch and bounce paths
verbatim. `tools/thumbtest.cpp` is the evidence probe for the DWM destination style:
it makes **its own** demo windows, screenshots what the compositor actually drew and
counts handles across register/unregister cycles — the same "don't touch the live
dock" approach as the genie selftest. **It was never compiled or run**, so nothing
here is confirmed by running code.

**No changes to `halo.cpp`, `dock.cpp` or `apps.cpp`** — both 3a and 3b are still
open. The collision check came back clean: its planned `WM_APP+4` and timer id 2 do
not collide with the genie branch, which uses registered messages.

**Nine design questions are waiting on the owner** (full text in
`docs/NOTES-previews.md`): card size, spacing and gap from the icon; window titles,
and anything for single-window apps; appear/disappear animation; hover delay before
showing; many windows (cap, scroll or shrink); what a click on a preview raises —
noting 3b changes what an icon click does; interaction with magnification; whether
square-cornered v1 cards are acceptable if regions don't clip DWM thumbnails; and
whether minimized windows get a preview at all.

### Memory: investigated, not fixed (untracked in the main checkout)

`docs/MEMORY.md` and `tools/memprobe.cpp` are untracked in the main checkout;
`memprobe.cpp` is not wired into `tools/build.sh`. This closes the housekeeping item.

- **The README's 17.5 MB does not reproduce.** Three separate cold starts of the
  current build read **32.7 / 31.7 / 32.4 MB** private. The figure is stale, and
  this build's floor is ~30 MB — which matches this file's own note that ~30.5 MB
  predates the polish work. Readings also swing ~5 MB while idle, so the README
  should state a range rather than a single number.
- **Cause:** `Halo::MakeIconBrush` sizes the surface correctly (44 px, ~13 KB) but
  paints it from the **full 256 px** source, costing **~277 KB of private commit per
  icon** (~7.3 MB across 27) and again on every repaint. `m_brushes` never evicts, so
  every distinct app path adds ~277 KB for the life of the process.
- **Proven dead ends, so nobody retries them:** interpolation mode,
  `ClearResources()`, `SetMaximumTextureMemory()`, and releasing the whole graphics
  stack (10.4 → 15.9 MB, worse). **An LRU on `m_brushes` would not fix it.**
- **Fix available:** draw from a source pre-shrunk to the magnified size →
  **30 KB per icon (9× less)**, and refreshes stop accumulating. It moves the 256→44
  downscale from D2D's cubic filter to a CPU one, so pixels change slightly —
  **the owner's call**; the options are in `docs/MEMORY.md`.

## Owner's complaints, and their status

| Complaint | Status |
|---|---|
| Genie isn't smooth | **IN FLIGHT.** v1 renders on the CPU (D2D DC render target + UpdateLayeredWindow, timer-paced). The GPU rewrite is branch `agent-genie-salvage` (task 1) — see *Work in flight*. |
| Can't see an app's multiple windows on hover | **OPEN.** Task 3. |
| No bobbing when clicking an icon | Fixed in `7f95d16`. `Settle` was killing it within 120 ms, and ShellExecute blocked before `Bounce` ran. |
| Tray/clock pill looked bad | Removed. A status cluster and Control Center replace it (tasks 2 and 4). |
| Duplicate running icons | Fixed in `494c216`. |
| Taskbar came back after explorer restart | Fixed in `d6f1402`. |
| MS Paint opening every 15 s | It was a test probe, not a bug. Stopped. |
| Smart App Control blocks unsigned builds | **OPEN.** Needs code signing. Undecided. |
| Memory 36.5 MB private (README says 17.5) | **INVESTIGATED, NOT FIXED.** The README number is stale — three cold starts now read ~32 MB, so the floor is ~30 MB. Cause, dead ends and the 9× fix are in `docs/MEMORY.md`; the fix changes the downscale filter, so it waits on the owner. |

## Next, in priority order

1. **Smooth GPU genie.**
   - **Plan:** the mod only captures the window and hands the pixels over through shared memory. Halo renders a strip-based genie on the GPU with Composition keyframes.
   - **Existing work:**
     - WIP on branch `worktree-agent-a8192aa49071b9b8d`, commit `edb3933`: `src/genie.h`, the handoff protocol and a frame-capture benchmark. It was paused mid-edit and **may not build**.
     - A bench script is on `worktree-agent-a99d4293e409462d0`, commit `c69d20f`.
     - **Salvaged and extended** on `agent-genie-salvage` (11 commits) — see *Work in flight*. Both old worktrees can now be deleted once it lands.
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
   - Investigate memory. **Done** — `docs/MEMORY.md` (root cause plus a 9× fix awaiting the owner's filter call).
   - Test with a second monitor once one is plugged in.
   - **License:** MIT is recommended. The owner decides before the repo goes public.

## Worktrees

Agent branches live under `.claude/worktrees/` (untracked). Everything useful is
merged into `main` **except** the two branches now open: `agent-genie-salvage`
(salvages `a8192aa…` and the `a99d42…` bench; 11 commits) and `agent-previews`
(task 3; 2 commits, design only).

`a8192aa…` and `a99d42…` are therefore salvaged: those two, and the older merged
worktrees, can be deleted once `agent-genie-salvage` lands.

**Nothing here is merged into `main`.** Merging is the owner's call.
