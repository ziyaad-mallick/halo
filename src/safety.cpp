#include "safety.h"

#include <shellapi.h>

#include <atomic>
#include <cstdlib>
#include <exception>
#include <string>

#include "taskbar.h"

namespace halo::safety {
namespace {

std::atomic<bool> g_leaving{false};
std::atomic<int> g_respawns{0};
constexpr int kMaxRespawns = 3;

// A guardian that is running and pumping messages. The 500 ms covers a guardian that is blocked
// closing us; restoring a second time then is harmless.
bool GuardianAnswers() {
    HWND w = FindWindowW(L"HaloGuardianWnd", nullptr);
    if (!w) return false;
    DWORD_PTR r = 0;
    return SendMessageTimeoutW(w, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 500, &r) != 0;
}

void RestoreOnce() {
    if (g_leaving.exchange(true)) return;
    try { halo::taskbar::RestoreAll(); } catch (...) {}
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    RestoreOnce();
    return EXCEPTION_CONTINUE_SEARCH;  // let Windows end the process: the guardian sees the exit code
}

void WatchGuardian(DWORD pid);

// Runs on a thread-pool thread when the guardian process ends while we are still running.
void CALLBACK GuardianGone(PVOID, BOOLEAN) {
    if (g_leaving) return;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring dir(buf, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    std::wstring exe = dir + L"guardian.exe";
    std::wstring cmd = L"\"" + exe + L"\" --adopt " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (g_respawns++ < kMaxRespawns &&
        CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        WatchGuardian(pi.dwProcessId);
        return;
    }
    RestoreOnce();  // no guardian and none can start: leave Windows' taskbar next to the dock
}

void WatchGuardian(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return;
    HANDLE wait = nullptr;
    // The handle stays open for the life of the process: the wait needs it.
    if (!RegisterWaitForSingleObject(&wait, h, GuardianGone, nullptr, INFINITE, WT_EXECUTEONLYONCE)) CloseHandle(h);
}

}  // namespace

void Install() {
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate([] {  // an uncaught C++ / WinRT exception
        RestoreOnce();
        std::abort();
    });
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i)
            if (!_wcsicmp(argv[i], L"--guardian")) WatchGuardian(static_cast<DWORD>(_wtoi(argv[i + 1])));
        LocalFree(argv);
    }
}

void Leave(bool crashed) {
    if (crashed || !GuardianAnswers()) RestoreOnce();
    g_leaving = true;  // from here a guardian exit is expected, not a death
}

}  // namespace halo::safety
