# guardian-logoff.patch

Against `36e63d1`, `src/guardian.cpp` only. Not applied here because that file is being changed in another worktree.
Apply with `git apply docs/patches/guardian-logoff.patch` (checked: `git apply --check` is clean on 36e63d1; not compiled).

## The bug

At sign-out, restart or shutdown Windows kills halo.exe (exit `0x40010004`, DBG_TERMINATE_PROCESS) before it delivers
`WM_ENDSESSION` to the guardian. The guardian saw a dead halo, counted a crash and relaunched it 1 s later; that process
cannot start during logoff (`0xC000026B`, STATUS_DLL_INIT_FAILED_LOGOFF), twice, so the third failure in two minutes
made it "give up" (and, without `--noui`, show a "stopped after repeated crashes" box in a dying session). Seen on
20 Oct, 25 Oct, 26 Oct, 11 Nov 23:25 and 12 Nov 22:59.

## The change

- `sessionEnding_`, set in `WM_QUERYENDSESSION`, cleared again on `WM_ENDSESSION` with `wParam == 0` (another app vetoed it).
- `SessionEnding()` = that flag or `GetSystemMetrics(SM_SHUTTINGDOWN)`, which covers the gap before the guardian's own query arrives.
- `OnHaloExit`: if the session is ending, or the exit code is `0x40010004` / `0xC000026B`, restore the taskbar, do not
  count a crash, do not relaunch.
- `Launch()` and the relaunch timer refuse while the session is ending.
- After a veto the guardian relaunches halo if it was ON and halo is gone.

A bare `0x40010004` with no logoff (a debugger killing halo) also goes OFF without a relaunch; Win+Alt+T turns it back on.

halo.exe side (done in `src/halo.cpp`): `WM_QUERYENDSESSION` agrees, `WM_ENDSESSION(TRUE)` restores the taskbar if the
guardian is gone and calls `ExitProcess(0)`, which the current guardian already reads as "closed", so the installed
guardian.exe stops mis-counting these as soon as the new halo.exe is installed.
