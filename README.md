# Halo

A native, macOS-style dock for Windows 11, written in C++/WinRT on
Windows.UI.Composition. It replaces the taskbar with a frosted-glass dock
whose icons magnify under the cursor, all animated by the compositor.

Status: **daily driver on one machine** (the author's login shell since 2026-10-18); not packaged or signed yet. See [docs/DESIGN.md](docs/DESIGN.md).

## What works

- Frosted pill (host backdrop blur), 256px icons for pinned apps, click to launch
- Magnification and a pill that grows with it, driven by compositor expressions
- Auto-hide with a spring reveal
- Light frosted theme by default (`HALO_THEME=dark` for the dark glass); all colours in `src/theme.h`
- Halo's own pin list (`%LOCALAPPDATA%\Halo\pins.txt`, seeded once from the taskbar pins) and a right-click menu: open windows, Keep in Dock / Remove from Dock, Show in Explorer, Quit
- Left status cluster (time, Wi-Fi, battery, Start), hover labels, live window previews, click-to-minimize, bounce on launch
- Start panel (Win key): recent apps and files, power menu, stacked with the PowerToys Command Palette
- Control Center (Win+A or the top-right corner): Wi-Fi/Bluetooth, toggles, brightness and volume, notifications and a calendar
- `guardian.exe` hides the Windows taskbar, runs the dock, and always gives the
  taskbar back: on exit, on a crash loop (3 crashes in 2 minutes), and after
  an explorer restart it hides the new taskbar again

Measured on a Core Ultra 7 258V, live login copy with the full feature set
(2026-10-18, process counters over 30 s idle): **16 ms CPU, 29.9 MB private
memory, 62 MB working set, 15 threads**; guardian 1.4 MB private.
Earlier M0 spike (2026-10-17, `tools/smoke.ps1`): 17.5 MB
private memory, 0 ms CPU over 10 s idle in 3 of 4 runs (250 ms in the first
run after an explorer restart, unexplained).

## Build

Needs only the LLVM-MinGW clang that ships with Windhawk (no Windows SDK):

```sh
tools/build.sh all      # build/halo.exe, build/guardian.exe
build/guardian.exe      # start ON: dock running, taskbar hidden
build/guardian.exe --restore   # get the taskbar back if anything goes wrong
```

`guardian.exe` stays alive as an always-on switch (zero CPU while OFF):

- **Win+Alt+T** toggles ON (dock, taskbar hidden) and OFF (dock closed, taskbar restored).
  If Windows refuses the hotkey (error 1409 happens on some builds) the guardian falls back
  to a low-level keyboard hook; `%LOCALAPPDATA%\Halo\guardian.log` says which one it used.
- Halo exiting by itself (Ctrl+Win+Esc, closed) means OFF, not exit. A crash loop (3 in
  2 minutes) also ends in OFF; Win+Alt+T tries again.
- A second `guardian.exe` talks to the running one: no flags = turn ON, `--off` (or
  `--restore`) = turn OFF, `--quit` = turn OFF and exit. With none running, `--off`/`--restore`
  just restore the taskbar.
- `guardian.exe --autostart on|off` adds/removes the `Halo` value under
  `HKCU\...\CurrentVersion\Run`.

While Halo runs, tapping the Windows key alone opens PowerToys Command Palette
(sends Win+Alt+Space) and reveals the dock instead of opening Start. Win+<key> combos are
untouched; Ctrl+Esc still opens Start. Set `HALO_TEST_WINKEY=1` to let injected Win presses
count (for tests).

Unsigned builds can be blocked by Smart App Control.
