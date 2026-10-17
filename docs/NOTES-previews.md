# Notes: hover previews (task 3a) + front-click minimize (task 3b) — work in progress, saved on interruption

*2026-10-17, branch `agent-previews`, off `main` at `9b9607a`. Written by the previews subagent
while exploring; nothing here is authoritative — `docs/HANDOFF.md` and `docs/DESIGN.md` are.*

## Status when interrupted

- Worktree created off `main` (`9b9607a`).
- Repo explored: `src/halo.cpp`, `src/dock.cpp/.h`, `src/apps.cpp/.h`, `tools/build.sh/docs/MEMORY.md` (main checkout), `docs/DESIGN.md`.
- Collision check done against `agent-genie-salvage`'s halo.cpp hunks (see "Coordination" below).
- `tools/thumbtest.cpp` written — a standalone probe for the DWM-thumbnail destination styles.
  **It has NOT been compiled or run yet.** The plan for running it is below. It creates its own
  demo windows (no external apps, nothing on a repeating loop with respect to the user's system;
  windows are destroyed before exit), so it should not disturb the live dock beyond a ~3 s flash
  of four small topmost test windows on the primary monitor's upper half.
- The previews module itself, the halo.cpp seams, the apps.cpp accessor, the Dock::IconRect
  accessor, and 3b are NOT yet implemented. No halo build was attempted (no changes that build).

## Proposed seam for 3a (not yet written)

New module `src/previews.h` (header-only, included by halo.cpp — note: `tools/build.sh` is
off-limits in this handoff, so a new .cpp can't be added to the compile list without the lead's
one-line change; header-only avoids touching build.sh. On merge the lead may move it to
`src/previews.cpp` and add one line to build.sh).

- `class WindowPreviews` owns: a topmost `WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE`
  preview window created per show (destroyed on hide), the DWM thumbnail handles registered
  only while visible, and a WinEvent hook (active only while visible) watching
  destroy/hide/cloak/minimize of the previewed HWNDs → hides the preview.
- Geometry: Halo already owns everything. Per call it passes a small geo struct
  (dock window rect `m_full`, monitor rect, pill-top screen y, scale). Anchor = hovered icon's
  current center X. Cards: fixed height, width per card from the source client aspect (clamped
  to a max width), row clamped to the monitor. Destination rect for DWM thumbnails is in client
  coords of the preview window.
- Choreography (the fiddly bit):
  - `WM_MOUSEMOVE` (dock): Halo computes `HitTest(mx, my)` → forwards hit index + app key to
    `previews.OnHover(...)`. A qualifying app (≥2 previewable windows) starts a one-shot
    show delay via a Halo-owned `SetTimer` (timer id 2; **not** the existing kHideTimer id 1),
    killed on any hover change/leave. Design decision below on the delay length.
  - `WM_MOUSELEAVE` (dock) and a new `WM_APP+4` "pointer away" message sent by the previews
    window when the pointer leaves it for good both funnel into a slightly extended
    `Halo::PointerLeave()`: if the previews are visible and cover the cursor, keep the dock up;
    else hide previews + the existing hide path.
  - Preview `WM_MOUSEMOVE`s are forwarded to the dock as mapped `WM_MOUSEMOVE`s so
    magnification keeps tracking the icon under the cursor; `OnHover(-1)` while the cursor is
    inside the preview window is ignored (previews must not hide themselves from inside).
  - Previews are hidden on: dock leave, pointer leaves both icon and preview, icon change,
    app-set change (`kMsgAppsChanged` → `RefreshDock` can reorder icons), dock hide
    (`StartHide`), click on the icon (`Activate`), source window events (destroy/hide/cloak/minimize),
    and process exit (`WM_DESTROY`). Every registered thumbnail is explicitly
    `DwmUnregisterThumbnail`-ed before the preview window is destroyed; no thumb handles are
    kept while hidden.
- Sources come from a new `WindowTracker::WindowsOf(key)` (3-line addition to apps.cpp/.h —
  apps.cpp is not in the genie agent's file list). MRU first, as in the tracker.
- Filter: skip cloaked (`DWMWA_CLOAKED`), zero-size client, and windows whose
  `DwmRegisterThumbnail` fails. Not decided (question for owner): minimized windows. The brief
  says skip minimized; the Win taskbar previews minimized windows fine, so the thumbtest plans
  to test what a minimized source actually renders before deciding.
- Cap on window count: constant, default 6, MRU order (question for owner).

## 3b sketch (not yet written)

`Halo::Activate` rework, preserving everything else:

```cpp
HWND w = it.key.empty() ? nullptr : m_tracker->Front(it.key);
if (!w) { /* launch path EXACTLY as today, incl. Bounce before ShellExecute (7f95d16) */ return; }
HWND fg = GetForegroundWindow();
if (fg && m_tracker->KeyOf(fg) == it.key) {
    // App is already in front → minimize all its non-iconic tracked windows.
    // Mirrors the restore path: SendMessageTimeout WM_SYSCOMMAND SC_MINIMIZE (not ShowWindow)
    // so it runs in the app and the genie mod can animate it.
    for (HWND h : m_tracker->WindowsOf(it.key))
        if (!IsIconic(h)) SendMessageTimeoutW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 200, nullptr);
    return;
}
// Not in front → today's activate path (restore if iconic + SetForegroundWindow).
```

Note: `docs/DESIGN.md` "Dock decisions" table already records the owner's decision for this
("Click on the frontmost app → Minimize it (the Windows habit)"), so this is not a design
question — only the multi-window details (below) are.

## thumbtest plan (written, not yet built/run)

`tools/thumbtest.exe <outdir>`: creates demo content windows in-process (visible ~2.6 s), then
per destination style registers `DwmRegisterThumbnail` cards and saves PNGs of what the
compositor actually drew (screen-BitBlt + PrintWindow PW_RENDERFULLCONTENT), 800 ms and +1.4 s
apart (liveness = different frames), plus:

- Variant A: plain `WS_POPUP` + `DwmExtendFrameIntoClientArea(-1)` (aero-peek glass), cards for a
  visible demo, a second demo, and a **minimized** demo (what does a minimized source render?).
- Variant C: plain + `WS_EX_LAYERED` + `UpdateLayeredWindow` per-pixel alpha bg + thumbs.
- Variant D: `WS_EX_NOREDIRECTIONBITMAP` + Windows.UI.Composition cards (the dock's own stack).
  THE key unknown: whether DWM thumbnails composite at all into a NOREDIRECTIONBITMAP
  destination, and whether they land above or below composition content.
- Variant F: plain + rounded `SetWindowRgn` over the cards (does a window region clip thumbs?
  If yes, rounded cards are achievable cheaply — otherwise thumbs are square corners).
- Registration-while-hidden test (registration before `ShowWindow`, preferred for no flash).
- Register-then-destroy source test (HRESULT evidence for the docs).
- 300 visible + 300 hidden register/unregister cycles with
  `GetProcessHandleCount` / `GetGuiResources` deltas before/after (leak evidence).

Build (not touching tools/build.sh; mirrors its flags):

```sh
CXX=/c/Users/testi/Windhawk/Compiler/bin/clang++.exe
"$CXX" -std=c++23 -O2 -target x86_64-w64-mingw32 -municode -DUNICODE -D_UNICODE \
  -DWINVER=0x0A00 -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000008 -Wall \
  tools/thumbtest.cpp -o build/thumbtest.exe \
  -lwindowsapp -lruntimeobject -lcoremessaging -ld3d11 -ldxgi -ld2d1 -ldwrite -ldcomp -ldwmapi \
  -lole32 -luuid -luser32 -lgdi32 -lshell32 -lshcore -lcomctl32 -lwindowscodecs
```

Then read the PNGs from `C:\Users\testi\AppData\Local\Temp\opencode\thumbshots\` (code read via
file viewer) and pick the destination style from evidence.

## Coordination with `agent-genie-salvage`

Their halo.cpp hunks (read from their branch diff): includes, `kGenieHoldMs` 700→900,
`m_genieOpMsg` = `RegisterWindowMessageW(L"Halo.Genie")` (registered messages take their own
values — no `WM_APP+n` collision), `m_genie` member, one `OnMessage` line. My planned shared-file
seams do not overlap those lines; my numbers: `WM_APP+4` (unused), timer id 2 (kHideTimer is 1).
I do not touch `src/genie.*`, `src/interop.h`, `mods/`, `tools/build.sh`.

## Design questions collected for the owner (so far)

1. Preview card size, spacing between cards, and the gap from the dock icon
   (my lean: card height ≈ 150–190 logical px, gap 10–12, row anchored its gap above the pill top,
   centered on the hovered icon and clamped to the monitor).
2. Whether to show window titles, and whether a single-window app should show anything at all
   (the complaint is multi-window; macOS shows a card for single-window apps too; my lean: no
   titles in v1, show single-window cards too — but that is his call).
3. Appear/disappear animation: instant, or matched to the dock's presence spring
   (my lean: short fade/surge matching kPresenceMs, but instant-hide).
4. Hover delay before previews appear (my lean ~250–300 ms so sweeps don't flicker; HALF-test
   instant under HALO_TEST).
5. Many windows: cap at N (my lean 6, MRU order) vs scroll vs shrink.
6. What a click on a preview does (my lean: raise that window — non-activating/no-op swallow
   until decided; note 3b changes what clicking the *icon* does; both multi-window front-click
   minimize details—minimize all vs most-recent—are also to confirm).
7. Interaction with magnification: does the preview follow the magnified icon (my lean: static
   while visible, re-anchored per icon change) and does hovering it keep the dock up (my lean: yes).
8. Rounded thumbnails: DWM thumbnails are axis-aligned rects; if variant F shows regions don't
   clip them, cards must be square or rounded only via the destination region — confirm
   square-cornered v1 is acceptable if so (visual).
9. Minimized windows shown in previews or skipped (brief says skip; Win taskbar previews them;
   thumbtest result pending — his call).

## Unfinished

- Everything in the two task sections above except what is written in the notes: the module,
  seams, WindowsOf, IconRect, the Activate change, the build, the probe run, and all
  verification. Nothing of any task was confirmed by running code yet.
