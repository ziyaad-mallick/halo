// Halo's crash recorder. Install() once at the top of wWinMain; from then on every way the process can die
// leaves one line in %LOCALAPPDATA%\Halo\crash.log and (for real crashes) a MiniDumpNormal in
// %LOCALAPPDATA%\Halo\dumps (the newest 3 are kept).
//
// What it hooks, and what each one preserves of the original exit code:
//   SetUnhandledExceptionFilter  access violations & co.   returns CONTINUE_SEARCH: the process dies with the exception code.
//   vectored handler             first-chance only: stack overflow, heap corruption, fatal user-callback exception
//                                (0xC000041D), invalid CRT parameter. Also remembers the last first-chance error code
//                                (an AV swallowed in a callback shows up later as 0xC000041D).
//   std::set_terminate           a C++ exception nobody caught (a throw out of a WndProc / noexcept / winrt event
//                                handler). Chains to the previous handler, which aborts: 0xC0000409 as before.
//   signal(SIGABRT)              abort() from anywhere. The handler returns, abort() goes on to fail fast as before.
//   _set_invalid_parameter_handler / _set_purecall_handler
//                                record, then fail fast with the code the CRT would have used.
//   winrt_throw_hresult_handler  remembers the last C++/WinRT hresult thrown (HRESULT + the throw site), handled or not.
// __fastfail() itself cannot be intercepted (the kernel kills the process); the terminate / abort / invalid-parameter
// hooks run just before the fail-fast, which is where Halo's 0xC0000409 deaths come from.
//
// Inside a handler: no CRT stdio, no new/delete except what the winrt message of a hresult_error needs (written after
// the main line, so a deadlock there still leaves the line). Strings are formatted into a stack buffer and written with
// CreateFile / WriteFile. Only the first fatal record of a process is written (g_busy is never released).
//
// Breadcrumbs: HALO_CRUMB("stage") stores a pointer to a string literal in a 16-slot ring (thread id + tick). Cheap
// enough for message handlers. The last few are printed on every record.
#pragma once
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <new>

#if !(defined(__x86_64__) || defined(_M_X64))
#error "crash.h walks the x64 PEB / unwind tables"
#endif

namespace halo::crash {
namespace detail {

struct CrumbSlot { char const* what; DWORD tid; DWORD tick; };
inline CrumbSlot g_ring[16];
inline volatile LONG g_ringPos = 0;
inline volatile UINT g_lastMsg = 0;

inline char g_log[8][200];  // the last few Log() lines, UTF-8
inline volatile LONG g_logPos = 0;

inline wchar_t g_dir[MAX_PATH];
inline wchar_t g_logPath[MAX_PATH];
inline wchar_t g_dumpDir[MAX_PATH];
inline volatile LONG g_installed = 0;
inline volatile LONG g_busy = 0;
inline volatile LONG g_sessionEnding = 0;
inline volatile LONG g_handled = 0;

struct FirstChance { DWORD code; DWORD tid; DWORD tick; uint64_t addr; };
inline FirstChance g_fc{};

struct WinrtErr { int32_t hr; uint32_t line; char const* file; char const* fn; DWORD tid; DWORD tick; };
inline WinrtErr g_wr{};

inline LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
inline std::terminate_handler g_prevTerminate = nullptr;

// ---- formatting -------------------------------------------------------------------------------------------------
struct Buf {
    char b[4096];
    size_t n = 0;
    void put(char c) { if (n + 1 < sizeof b) b[n++] = c; }
    void str(char const* s) {
        if (!s) { str("null"); return; }
        while (*s) put(*s++);
    }
    void hex(uint64_t v, int minDigits = 1) {
        char t[20];
        int k = 0;
        do { t[k++] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v || k < minDigits);
        while (k) put(t[--k]);
    }
    void dec(uint64_t v, int minDigits = 1) {
        char t[24];
        int k = 0;
        do { t[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v || k < minDigits);
        while (k) put(t[--k]);
    }
    void wstr(wchar_t const* s, int len = -1) {
        if (!s) { str("null"); return; }
        char t[600];
        int m = WideCharToMultiByte(CP_UTF8, 0, s, len, t, static_cast<int>(sizeof t) - 1, nullptr, nullptr);
        if (m <= 0) { str("?"); return; }
        if (len < 0 && m > 0) --m;  // the terminator was converted too
        for (int i = 0; i < m && t[i]; i++) put(t[i] == '\r' || t[i] == '\n' || t[i] == '\t' ? ' ' : t[i]);
    }
    void stamp() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        dec(st.wYear, 4); put('-'); dec(st.wMonth, 2); put('-'); dec(st.wDay, 2); put(' ');
        dec(st.wHour, 2); put(':'); dec(st.wMinute, 2); put(':'); dec(st.wSecond, 2); put('.'); dec(st.wMilliseconds, 3);
    }
};

// ---- module lookup: a lock-free walk of the loader list (the loader lock may be what we crashed under) ----------------
struct UStr { USHORT len, max; PWSTR buf; };
struct LdrEntry {  // x64 LDR_DATA_TABLE_ENTRY, as far as we need it
    LIST_ENTRY inLoadOrder, inMemoryOrder, inInitOrder;
    PVOID dllBase, entryPoint;
    ULONG sizeOfImage;
    UStr fullName, baseName;
};

inline bool ModuleFor(uint64_t addr, uint64_t& base, wchar_t const*& name, int& nameChars) {
    auto* teb = reinterpret_cast<char*>(NtCurrentTeb());
    auto* peb = *reinterpret_cast<char**>(teb + 0x60);
    if (!peb) return false;
    auto* ldr = *reinterpret_cast<char**>(peb + 0x18);
    if (!ldr) return false;
    auto* head = reinterpret_cast<LIST_ENTRY*>(ldr + 0x10);  // InLoadOrderModuleList
    int guard = 0;
    for (LIST_ENTRY* e = head->Flink; e && e != head && guard < 512; e = e->Flink, ++guard) {
        auto* m = reinterpret_cast<LdrEntry*>(e);
        auto b = reinterpret_cast<uint64_t>(m->dllBase);
        if (b && addr >= b && addr < b + m->sizeOfImage) {
            base = b;
            name = m->baseName.buf;
            nameChars = m->baseName.len / 2;
            return true;
        }
    }
    return false;
}

inline void Sym(Buf& b, uint64_t a) {
    uint64_t base = 0;
    wchar_t const* name = nullptr;
    int nc = 0;
    if (ModuleFor(a, base, name, nc) && name) {
        b.wstr(name, nc);
        b.str("+0x");
        b.hex(a - base);
    } else {
        b.str("0x");
        b.hex(a);
    }
}

inline void Backtrace(Buf& b, CONTEXT ctx, int max, int skip) {
    for (int i = 0; i < max; i++) {
        DWORD64 pc = ctx.Rip;
        if (!pc) break;
        if (i >= skip) { if (i > skip) b.put(';'); Sym(b, pc); }
        DWORD64 base = 0;
        auto* fe = RtlLookupFunctionEntry(pc, &base, nullptr);
        if (!fe) break;  // a leaf without unwind data: nothing safe to read
        PVOID handlerData = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, fe, &ctx, &handlerData, &frame, nullptr);
        if (!ctx.Rsp) break;
    }
}

// ---- files ------------------------------------------------------------------------------------------------------
inline void MakeDirs(wchar_t const* path) {
    wchar_t tmp[MAX_PATH];
    lstrcpynW(tmp, path, MAX_PATH);
    for (wchar_t* p = tmp + 3; *p; ++p) {  // skip "C:\"
        if (*p == L'\\' || *p == L'/') {
            wchar_t c = *p;
            *p = 0;
            CreateDirectoryW(tmp, nullptr);
            *p = c;
        }
    }
    CreateDirectoryW(tmp, nullptr);
}

inline void AppendLog(char const* data, size_t n) {
    if (!g_logPath[0]) return;
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, data, static_cast<DWORD>(n), &w, nullptr);
    CloseHandle(h);
}

struct DumpEnt { FILETIME t; wchar_t name[96]; };
inline DumpEnt g_ents[64];

// Deletes the oldest *.dmp files until at most `keep` remain.
inline void PruneDumps(int keep) {
    if (!g_dumpDir[0]) return;
    wchar_t pat[MAX_PATH + 8];
    lstrcpynW(pat, g_dumpDir, MAX_PATH);
    lstrcatW(pat, L"\\*.dmp");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    int n = 0;
    do {
        if (n < 64 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            g_ents[n].t = fd.ftLastWriteTime;
            lstrcpynW(g_ents[n].name, fd.cFileName, 96);
            ++n;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    while (n > keep) {
        int old = 0;
        for (int i = 1; i < n; i++)
            if (CompareFileTime(&g_ents[i].t, &g_ents[old].t) < 0) old = i;
        wchar_t full[MAX_PATH + 100];
        lstrcpynW(full, g_dumpDir, MAX_PATH);
        lstrcatW(full, L"\\");
        lstrcatW(full, g_ents[old].name);
        DeleteFileW(full);
        g_ents[old] = g_ents[--n];
    }
}

inline bool WriteDump(wchar_t const* path, EXCEPTION_POINTERS* ep, DWORD* err) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { *err = GetLastError(); return false; }
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, ep ? &mei : nullptr,
                                nullptr, nullptr);
    *err = ok ? 0 : GetLastError();
    CloseHandle(f);
    return ok != FALSE;
}

inline void AppendCrumbs(Buf& b) {
    LONG pos = g_ringPos;
    DWORD now = GetTickCount();
    b.str(" crumbs=");
    bool any = false;
    for (int k = 0; k < 4; k++) {
        auto const& c = g_ring[(pos - k) & 15];
        if (!c.what) continue;
        if (any) b.put('<');
        any = true;
        b.str(c.what);
        b.str("@t"); b.dec(c.tid); b.str("-"); b.dec(now - c.tick); b.str("ms");
    }
    if (!any) b.str("none");
    b.str(" lastmsg=0x"); b.hex(g_lastMsg);
}

inline void AppendWinrt(Buf& b) {
    if (!g_wr.tick) return;
    b.str(" winrt=0x"); b.hex(static_cast<uint32_t>(g_wr.hr), 8);
    b.str("@t"); b.dec(g_wr.tid); b.str("-"); b.dec(GetTickCount() - g_wr.tick); b.str("ms ");
    b.str(g_wr.file ? g_wr.file : "?");
    b.put(':'); b.dec(g_wr.line);
    if (g_wr.fn) { b.put(' '); b.str(g_wr.fn); }
}

inline void AppendLogTail(Buf& b) {
    LONG pos = g_logPos;
    for (int k = 7; k >= 0; k--) {
        char const* l = g_log[(pos - k) & 7];
        if (!l[0]) continue;
        b.str("\n  log: ");
        b.str(l);
    }
}

// The one record. `ep` may be null (terminate / abort / CRT handlers): the context is then captured here.
// `fault` is the address to report when there is no exception record.
inline void Record(char const* kind, DWORD code, EXCEPTION_POINTERS* ep, uint64_t fault, bool wantDump) {
    if (InterlockedExchange(&g_busy, 1)) return;  // first fatal record wins; a second one is the same death
    CONTEXT ctx;
    EXCEPTION_RECORD er;
    EXCEPTION_POINTERS synth;
    if (!ep) {
        RtlCaptureContext(&ctx);
        ZeroMemory(&er, sizeof er);
        er.ExceptionCode = code;
        er.ExceptionAddress = reinterpret_cast<PVOID>(fault ? fault : ctx.Rip);
        synth.ExceptionRecord = &er;
        synth.ContextRecord = &ctx;
        ep = &synth;
    }
    uint64_t addr = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
    const bool ending = g_sessionEnding != 0;

    wchar_t dumpPath[MAX_PATH + 100];
    char dumpName[64];
    dumpPath[0] = 0;
    dumpName[0] = 0;
    SYSTEMTIME st;
    GetLocalTime(&st);
    {
        Buf nb;
        nb.str("halo-"); nb.dec(st.wYear, 4); nb.dec(st.wMonth, 2); nb.dec(st.wDay, 2); nb.put('-');
        nb.dec(st.wHour, 2); nb.dec(st.wMinute, 2); nb.dec(st.wSecond, 2); nb.put('-'); nb.dec(GetCurrentProcessId());
        nb.str(".dmp");
        nb.b[nb.n] = 0;
        if (wantDump && !ending && g_dumpDir[0] && nb.n < sizeof dumpName) {
            for (size_t i = 0; i <= nb.n; i++) dumpName[i] = nb.b[i];
            lstrcpynW(dumpPath, g_dumpDir, MAX_PATH);
            lstrcatW(dumpPath, L"\\");
            wchar_t wn[64];
            for (size_t i = 0; i <= nb.n; i++) wn[i] = static_cast<wchar_t>(nb.b[i]);
            lstrcatW(dumpPath, wn);
        }
    }

    Buf b;
    b.stamp();
    b.str(" kind="); b.str(kind);
    b.str(" code=0x"); b.hex(code, 8);
    b.str(" addr=0x"); b.hex(addr, 16);
    b.str(" at="); Sym(b, addr);
    {
        uint64_t base = 0;
        wchar_t const* nm = nullptr;
        int nc = 0;
        if (ModuleFor(addr, base, nm, nc)) { b.str(" modbase=0x"); b.hex(base, 12); }
    }
    if (code == 0xC0000005 && ep->ExceptionRecord->NumberParameters >= 2) {
        b.str(" av="); b.str(ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "exec");
        b.str("@0x"); b.hex(ep->ExceptionRecord->ExceptionInformation[1]);
    }
    b.str(" pid="); b.dec(GetCurrentProcessId());
    b.str(" tid="); b.dec(GetCurrentThreadId());
    b.str(" session_ending="); b.dec(ending ? 1 : 0);
    AppendCrumbs(b);
    if (g_fc.code) {
        b.str(" lastfirstchance=0x"); b.hex(g_fc.code, 8);
        b.str("@"); Sym(b, g_fc.addr);
        b.str(" t"); b.dec(g_fc.tid); b.str("-"); b.dec(GetTickCount() - g_fc.tick); b.str("ms");
    }
    AppendWinrt(b);
    b.str(" bt=");
    Backtrace(b, *ep->ContextRecord, 16, 0);
    b.str(" dump="); b.str(dumpName[0] ? dumpName : "none");
    b.put('\n');
    AppendLog(b.b, b.n);

    if (dumpPath[0]) {
        PruneDumps(2);  // the new one makes three
        DWORD err = 0;
        if (!WriteDump(dumpPath, ep, &err)) {
            Buf e;
            e.str("  dump failed err="); e.dec(err); e.put('\n');
            AppendLog(e.b, e.n);
        }
    }
    {
        Buf t;
        AppendLogTail(t);
        if (t.n) { t.put('\n'); AppendLog(t.b, t.n); }
    }
}

// What is being thrown, for the terminate handler and for Handled(). Must run inside a catch or with an active exception.
inline void Describe(Buf& b) {
    if (!std::current_exception()) { b.str("no current exception"); return; }
    try {
        throw;
    }
#ifdef WINRT_BASE_H
    catch (winrt::hresult_error const& e) {
        b.str("winrt::hresult_error hr=0x"); b.hex(static_cast<uint32_t>(static_cast<int32_t>(e.code())), 8);
        b.str(" message="); b.wstr(e.message().c_str());
    }
#endif
    catch (std::bad_alloc const&) { b.str("std::bad_alloc"); }
    catch (std::exception const& e) { b.str("std::exception what="); b.str(e.what()); }
    catch (...) { b.str("unknown exception type"); }
}

[[noreturn]] inline void FastFail(unsigned code) {
    __asm__ volatile("int $0x29" : : "c"(code) : "memory");
    __builtin_unreachable();
}

inline LONG WINAPI Uef(EXCEPTION_POINTERS* ep) {
    Record("unhandled-exception", ep->ExceptionRecord->ExceptionCode, ep, 0, true);
    if (g_prevFilter) return g_prevFilter(ep);
    return EXCEPTION_CONTINUE_SEARCH;  // WER and the exit code stay as they were
}

inline LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    switch (c) {
        case 0xC00000FD:  // stack overflow
        case 0xC0000374:  // heap corruption
        case 0xC0000409:  // stack buffer overrun / fast fail raised as an exception
        case 0xC0000417:  // invalid CRT parameter
        case 0xC000041D:  // an exception escaped a user32 callback (WndProc / hook)
            Record("fatal-first-chance", c, ep, 0, true);
            break;
        default:
            if ((c >> 28) == 0xC) {  // error severity only: no breakpoints, guard pages, debug strings, C++ throws
                g_fc.code = c;
                g_fc.tid = GetCurrentThreadId();
                g_fc.tick = GetTickCount();
                g_fc.addr = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
            }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

inline void Terminate() {
    Record("terminate", 0xC0000409, nullptr, reinterpret_cast<uint64_t>(__builtin_return_address(0)), true);
    {
        Buf b;
        b.str("  exception: ");
        try { Describe(b); } catch (...) {}
        b.put('\n');
        AppendLog(b.b, b.n);
    }
    if (g_prevTerminate) g_prevTerminate();  // libc++abi: message, then abort() -> fail fast, 0xC0000409
    std::abort();
}

inline void __cdecl OnAbort(int) {
    Record("abort", 0xC0000409, nullptr, reinterpret_cast<uint64_t>(__builtin_return_address(0)), true);
    // returning lets abort() carry on exactly as before
}

inline void __cdecl OnInvalidParameter(wchar_t const* expr, wchar_t const* fn, wchar_t const* file, unsigned line, uintptr_t) {
    Record("invalid-parameter", 0xC0000417, nullptr, reinterpret_cast<uint64_t>(__builtin_return_address(0)), true);
    Buf b;
    b.str("  invalid parameter: expr="); b.wstr(expr); b.str(" fn="); b.wstr(fn); b.str(" file="); b.wstr(file);
    b.str(" line="); b.dec(line); b.put('\n');
    AppendLog(b.b, b.n);
    FastFail(5);  // FAST_FAIL_INVALID_ARG: what the CRT does when no handler is installed
}

inline void __cdecl OnPurecall() {
    Record("pure-virtual-call", 0xC0000409, nullptr, reinterpret_cast<uint64_t>(__builtin_return_address(0)), true);
    FastFail(7);  // FAST_FAIL_FATAL_APP_EXIT: the same 0xC0000409 the CRT's own abort path ends in
}

#ifdef WINRT_BASE_H
inline void __stdcall OnWinrtThrow(uint32_t line, char const* file, char const* fn, void*, winrt::hresult const result) noexcept {
    g_wr.hr = static_cast<int32_t>(result);
    g_wr.line = line;
    g_wr.file = file;
    g_wr.fn = fn;
    g_wr.tid = GetCurrentThreadId();
    g_wr.tick = GetTickCount() | 1;  // never 0: 0 means "none yet"
}
#endif

}  // namespace detail

// Call first thing in wWinMain. `dir`: where crash.log and dumps\ go; null = %LOCALAPPDATA%\Halo.
inline void Install(wchar_t const* dir = nullptr) {
    using namespace detail;
    if (InterlockedExchange(&g_installed, 1)) return;
    if (dir && *dir) {
        lstrcpynW(g_dir, dir, MAX_PATH - 16);
    } else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", g_dir, MAX_PATH - 16);
        if (!n || n >= MAX_PATH - 16) {
            GetTempPathW(MAX_PATH - 16, g_dir);
            lstrcatW(g_dir, L"Halo");
        } else {
            lstrcatW(g_dir, L"\\Halo");
        }
    }
    MakeDirs(g_dir);
    lstrcpynW(g_logPath, g_dir, MAX_PATH);
    lstrcatW(g_logPath, L"\\crash.log");
    lstrcpynW(g_dumpDir, g_dir, MAX_PATH);
    lstrcatW(g_dumpDir, L"\\dumps");
    MakeDirs(g_dumpDir);
    {  // keep crash.log small: past 256 KB the old one moves aside
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(g_logPath, GetFileExInfoStandard, &fa) && fa.nFileSizeHigh == 0 && fa.nFileSizeLow > 256 * 1024) {
            wchar_t old[MAX_PATH + 8];
            lstrcpynW(old, g_logPath, MAX_PATH);
            lstrcatW(old, L".old");
            MoveFileExW(g_logPath, old, MOVEFILE_REPLACE_EXISTING);
        }
    }
    ULONG guarantee = 128 * 1024;  // room for the handler after a stack overflow on this thread
    SetThreadStackGuarantee(&guarantee);
    g_prevFilter = SetUnhandledExceptionFilter(&Uef);
    AddVectoredExceptionHandler(1, &Veh);
    g_prevTerminate = std::set_terminate(&Terminate);
    std::signal(SIGABRT, &OnAbort);
    _set_invalid_parameter_handler(&OnInvalidParameter);
    _set_purecall_handler(&OnPurecall);
#ifdef WINRT_BASE_H
    winrt_throw_hresult_handler = &OnWinrtThrow;
#endif
}

inline void Crumb(char const* what) {
    LONG i = InterlockedIncrement(&detail::g_ringPos);
    auto& c = detail::g_ring[i & 15];
    c.what = what;
    c.tid = GetCurrentThreadId();
    c.tick = GetTickCount();
}

// The window message being handled (a plain store: cheap enough for every message).
inline void Msg(UINT m) { detail::g_lastMsg = m; }

// Keeps the last 8 Log() lines for the record.
inline void NoteLog(wchar_t const* msg) {
    LONG i = InterlockedIncrement(&detail::g_logPos);
    char* dst = detail::g_log[i & 7];
    int len = 0;
    while (msg[len] && len < 150) ++len;
    int m = WideCharToMultiByte(CP_UTF8, 0, msg, len, dst, 198, nullptr, nullptr);
    if (m < 0) m = 0;
    for (int k = 0; k < m; k++) if (dst[k] == '\n' || dst[k] == '\r') dst[k] = ' ';
    dst[m] = 0;
}

// WM_ENDSESSION / WM_QUERYENDSESSION seen: a death from here on is the session ending, not a crash.
inline void SessionEnding() { detail::g_sessionEnding = 1; }
inline bool IsSessionEnding() { return detail::g_sessionEnding != 0; }

// An exception that was caught and survived (call inside the catch). Rate-limited: 25 per process.
inline void Handled(char const* site) {
    using namespace detail;
    if (!g_installed || InterlockedIncrement(&g_handled) > 25) return;
    try {
        Buf b;
        b.stamp();
        b.str(" kind=handled site="); b.str(site);
        b.str(" pid="); b.dec(GetCurrentProcessId());
        b.str(" tid="); b.dec(GetCurrentThreadId());
        AppendCrumbs(b);
        AppendWinrt(b);
        b.str(" exception: ");
        Describe(b);
        b.put('\n');
        AppendLog(b.b, b.n);
    } catch (...) {}
}

// Halo.Create() returned false (exit code 2): no crash, but nothing else says why.
inline void StartupFailed(char const* why) {
    using namespace detail;
    Buf b;
    b.stamp();
    b.str(" kind=startup-failed code=0x00000002 pid="); b.dec(GetCurrentProcessId());
    b.str(" why="); b.str(why);
    AppendCrumbs(b);
    AppendWinrt(b);
    AppendLogTail(b);
    b.put('\n');
    AppendLog(b.b, b.n);
}

}  // namespace halo::crash

#define HALO_CRUMB(s) ::halo::crash::Crumb(s)
