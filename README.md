# Halo

A native, macOS-style dock for Windows 11, written in C++/WinRT on
Windows.UI.Composition. It replaces the taskbar with a frosted-glass dock
whose icons magnify under the cursor, all animated by the compositor.

Status: **M0 spike**, not ready for daily use. See [docs/DESIGN.md](docs/DESIGN.md).

## What works

- Frosted pill (host backdrop blur), 256px icons for pinned apps, click to launch
- Magnification and a pill that grows with it, driven by compositor expressions
- Auto-hide with a spring reveal
- `guardian.exe` hides the Windows taskbar, runs the dock, and always gives the
  taskbar back: on exit, on a crash loop (3 crashes in 2 minutes), and after
  an explorer restart it hides the new taskbar again

Measured on a Core Ultra 7 258V (2026-10-17, `tools/smoke.ps1`): 17.5 MB
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
