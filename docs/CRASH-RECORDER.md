# Crash recorder

`src/crash.h`, installed first thing in `wWinMain`. Every death of halo.exe leaves a line in
`%LOCALAPPDATA%\Halo\crash.log` and, for real crashes, a `MiniDumpNormal` in `%LOCALAPPDATA%\Halo\dumps`
(newest 3 kept). The process still dies with its original exit code, so the guardian sees what it always saw.

## Reading a line

```
2026-11-15 13:42:11.123 kind=terminate code=0xC0000409 addr=0x... at=halo.exe+0x1234 modbase=0x... pid=.. tid=..
  session_ending=0 crumbs=Halo::RefreshDock@t1234-3ms<Halo::WndProc@t1234-3ms<... lastmsg=0x113
  lastfirstchance=0xC0000005@halo.exe+0x.. winrt=0x80070057@file:line fn bt=halo.exe+0x..;halo.exe+0x..;... dump=halo-20261115-134211-1234.dmp
  exception: winrt::hresult_error hr=0x80070057 message=The expression string is too long
  log: [halo] ...            (the last 8 Log() lines)
```

| kind | what died |
|---|---|
| `unhandled-exception` | access violation and other SEH exceptions (exit code = the exception code) |
| `terminate` | a C++ exception nobody caught: a throw out of a WndProc, a noexcept function, a hook or a std::thread. Exit 0xC0000409 |
| `abort` | `abort()` called from anywhere. Exit 0xC0000409 |
| `invalid-parameter`, `pure-virtual-call` | CRT checks. Exit 0xC0000409 |
| `fatal-first-chance` | stack overflow, heap corruption, 0xC000041D (an exception escaped a user32 callback) |
| `handled` | an exception that was caught and survived (`Guarded()` callbacks, the main WndProc). Not a death |
| `startup-failed` | `Halo::Create` returned false (exit 2). The `log:` lines name the step that failed |

`winrt=` is the last C++/WinRT HRESULT thrown anywhere in the process (handled or not) with its throw site. It is
the best lead for a `terminate` whose `exception:` line could not be read.

An exception escaping a `winrt::fire_and_forget` coroutine does **not** reach `std::terminate`: C++/WinRT calls
`RoFailFastWithErrorContext`, which no handler can intercept. It shows as a fail-fast with `winrt=` set and no record of
its own. Keep a try/catch around every coroutine body.

## Symbolising an offset

`at=` / `bt=` are `module+0xRVA`. For halo.exe: `llvm-nm -n --demangle halo.exe` (the release exe keeps its COFF symbol
table; the VA is `0x140000000 + RVA`), the linker map `build/halo.map` (always written next to the exe), or build with
`HALO_DEBUG=1 bash tools/build.sh halo` (the exe it leaves is stripped of DWARF but keeps its symbol table, and is smaller than a plain build) and run `llvm-addr2line -e build/halo.debug -f -C -a 0x<VA>` for file:line. The
dump opens in WinDbg / Visual Studio against the matching exe; keep the exe that crashed (match the PE timestamp).

## Pending patches in files this change does not own

`docs/patches/guardian-logoff.patch` (see `guardian-logoff.md`), plus the try/catch list in the audit report.
