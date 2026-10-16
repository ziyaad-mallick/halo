// guardian.exe - no UI. Only this process hides Windows' taskbar, and it
// guarantees the taskbar comes back whenever halo.exe goes away.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>

#include "taskbar.h"

namespace {

constexpr wchar_t kMutexName[] = L"Local\\HaloGuardian";
constexpr wchar_t kWndClass[] = L"HaloGuardianWnd";
constexpr DWORD kRelaunchDelayMs = 1000;
constexpr DWORD kHideAfterAliveMs = 3000;
constexpr DWORD kCrashWindowMs = 120000;
constexpr size_t kMaxCrashes = 3;
constexpr UINT_PTR kRehideTimer = 1;

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }
    void reset(HANDLE h = nullptr) {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = h;
    }
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

// ---- logging: one timestamped line per event -------------------------------
void Log(const wchar_t* fmt, ...) {
    try {
        wchar_t msg[512];
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf(msg, 511, fmt, ap);
        va_end(ap);
        msg[511] = 0;
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t line[640];
        int n = _snwprintf(line, 639, L"%04d-%02d-%02d %02d:%02d:%02d.%03d  %ls\r\n", t.wYear,
                           t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
        if (n <= 0) return;
        int len = WideCharToMultiByte(CP_UTF8, 0, line, n, nullptr, 0, nullptr, nullptr);
        std::string u8(static_cast<size_t>(len), '\0');
        WideCharToMultiByte(CP_UTF8, 0, line, n, u8.data(), len, nullptr, nullptr);
        std::wstring path = halo::taskbar::DataDir() + L"\\guardian.log";
        Handle f(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!f) return;
        DWORD w = 0;
        WriteFile(f.get(), u8.data(), static_cast<DWORD>(u8.size()), &w, nullptr);
    } catch (...) {
    }
}

void SafeHide() {
    try { halo::taskbar::HideAll(); } catch (...) { Log(L"HideAll threw"); }
}
void SafeRestore() {
    try { halo::taskbar::RestoreAll(); } catch (...) { Log(L"RestoreAll threw"); }
}

// ---- guardian ---------------------------------------------------------------
class Guardian {
public:
    bool noUi = false;

    int Run() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = &Guardian::WndProcThunk;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kWndClass;
        RegisterClassW(&wc);
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
        hwnd_ = CreateWindowExW(0, kWndClass, L"HaloGuardian", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                wc.hInstance, this);
        if (!hwnd_) Log(L"CreateWindowEx failed (%lu); continuing without message window", GetLastError());
        g_self = this;
        SetConsoleCtrlHandler(&Guardian::CtrlHandler, TRUE);

        Log(L"guardian started");
        SafeHide();
        hidden_ = true;
        Launch();

        while (!quit_) {
            DWORD timeout = INFINITE;
            DWORD now = GetTickCount();
            if (proc_ && pendingHide_) timeout = Remaining(hideAt_, now);
            if (!proc_ && relaunchPending_) timeout = Remaining(relaunchAt_, now);

            DWORD count = proc_ ? 1 : 0;
            HANDLE h = proc_.get();
            DWORD r = MsgWaitForMultipleObjects(count, count ? &h : nullptr, FALSE, timeout, QS_ALLINPUT);
            if (r == WAIT_FAILED) {
                Log(L"wait failed (%lu)", GetLastError());
                Sleep(100);
            }
            if (count && r == WAIT_OBJECT_0) OnHaloExit();

            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) quit_ = true;
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            now = GetTickCount();
            if (proc_ && pendingHide_ && Remaining(hideAt_, now) == 0) {
                pendingHide_ = false;
                SafeHide();
                hidden_ = true;
                Log(L"halo alive %lu ms, taskbar hidden", kHideAfterAliveMs);
            }
            if (!proc_ && relaunchPending_ && !quit_ && Remaining(relaunchAt_, now) == 0) {
                relaunchPending_ = false;
                Launch();
            }
        }
        Shutdown(L"loop exit");
        return exitCode_;
    }

private:
    static inline Guardian* g_self = nullptr;
    HWND hwnd_ = nullptr;
    UINT taskbarCreated_ = 0;
    Handle proc_;
    bool hidden_ = false, pendingHide_ = false, relaunchPending_ = false, quit_ = false, down_ = false;
    DWORD hideAt_ = 0, relaunchAt_ = 0;
    std::vector<DWORD> crashes_;
    int exitCode_ = 0;

    static DWORD Remaining(DWORD at, DWORD now) {
        LONG d = static_cast<LONG>(at - now);
        return d > 0 ? static_cast<DWORD>(d) : 0;
    }

    static std::wstring HaloPath() {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
        std::wstring p(buf, n);
        size_t s = p.find_last_of(L"\\/");
        return (s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1)) + L"halo.exe";
    }

    void Launch() {
        std::wstring path = HaloPath();
        std::wstring cmd = L"\"" + path + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            Log(L"failed to launch %ls (error %lu)", path.c_str(), GetLastError());
            // Treat a launch failure like a crash so we cannot spin forever.
            SafeRestore();
            hidden_ = false;
            RecordCrashAndDecide(L"launch failure");
            return;
        }
        CloseHandle(pi.hThread);
        proc_.reset(pi.hProcess);
        Log(L"halo launched, pid %lu", pi.dwProcessId);
        pendingHide_ = !hidden_;  // relaunch: hide again once alive for 3 s
        hideAt_ = GetTickCount() + kHideAfterAliveMs;
    }

    void OnHaloExit() {
        DWORD code = 1;
        if (!GetExitCodeProcess(proc_.get(), &code)) code = 1;
        proc_.reset();
        pendingHide_ = false;
        if (down_) return;
        if (code == 0) {
            Log(L"halo exited normally; restoring taskbar and exiting");
            SafeRestore();
            hidden_ = false;
            quit_ = true;
            return;
        }
        Log(L"halo died (exit code 0x%08lX); restoring taskbar", code);
        SafeRestore();
        hidden_ = false;
        RecordCrashAndDecide(L"crash");
    }

    void RecordCrashAndDecide(const wchar_t* why) {
        DWORD now = GetTickCount();
        crashes_.push_back(now);
        std::erase_if(crashes_, [now](DWORD t) { return now - t > kCrashWindowMs; });
        if (crashes_.size() >= kMaxCrashes) {
            Log(L"%zu failures (%ls) within 2 minutes; giving up, taskbar restored", crashes_.size(), why);
            SafeRestore();
            hidden_ = false;
            if (!noUi)
                MessageBoxW(nullptr, L"Halo stopped after repeated crashes — Windows' taskbar restored",
                            L"Halo", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            exitCode_ = 2;
            quit_ = true;
            return;
        }
        relaunchPending_ = true;
        relaunchAt_ = now + kRelaunchDelayMs;
        Log(L"relaunching halo in %lu ms", kRelaunchDelayMs);
    }

    // Terminate halo (if running) and restore the taskbar. Idempotent.
    void Shutdown(const wchar_t* why) {
        if (down_) return;
        down_ = true;
        Log(L"shutdown requested (%ls)", why);
        if (proc_) {
            TerminateProcess(proc_.get(), 0xDEAD);
            WaitForSingleObject(proc_.get(), 3000);
            proc_.reset();
        }
        SafeRestore();
        hidden_ = false;
        Log(L"taskbar restored; guardian exiting");
    }

    static BOOL WINAPI CtrlHandler(DWORD) {
        if (g_self && g_self->hwnd_) PostMessageW(g_self->hwnd_, WM_CLOSE, 0, 0);
        return TRUE;
    }

    static LRESULT CALLBACK WndProcThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<Guardian*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self) {
            try { return self->WndProc(h, m, w, l); } catch (...) { Log(L"exception in WndProc"); }
        }
        return DefWindowProcW(h, m, w, l);
    }

    LRESULT WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (taskbarCreated_ != 0 && m == taskbarCreated_) {
            Log(L"TaskbarCreated (explorer restarted)");
            if (proc_ && hidden_) {
                SafeHide();
                SetTimer(h, kRehideTimer, 1500, nullptr);  // taskbar may finish creating after the broadcast
            }
            return 0;
        }
        switch (m) {
            case WM_TIMER:
                if (w == kRehideTimer) {
                    KillTimer(h, kRehideTimer);
                    if (proc_ && hidden_) SafeHide();
                    return 0;
                }
                break;
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (w) {
                    Shutdown(L"logoff/shutdown");
                    quit_ = true;
                }
                return 0;
            case WM_CLOSE:
                Shutdown(L"WM_CLOSE");
                quit_ = true;
                return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};

bool HasFlag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], flag) == 0) return true;
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool restore = argv && HasFlag(argc, argv, L"--restore");
    const bool noUi = argv && HasFlag(argc, argv, L"--no-ui");
    if (argv) LocalFree(argv);

    if (restore) {
        Log(L"--restore: restoring native taskbar");
        SafeRestore();
        return 0;
    }

    Handle mutex(CreateMutexW(nullptr, TRUE, kMutexName));
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"another guardian is already running; exiting");
        return 1;
    }
    try {
        Guardian g;
        g.noUi = noUi;
        return g.Run();
    } catch (...) {
        Log(L"fatal exception; restoring taskbar");
        SafeRestore();
        return 3;
    }
}
