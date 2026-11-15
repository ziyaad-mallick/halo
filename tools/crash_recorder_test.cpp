// Crash recorder test. The parent re-runs itself as a child for each kind of death, with the recorder installed and
// its output directory pointed at a scratch folder, then checks what the child left behind:
//   exit code        the code the same death has without the recorder (0xC0000005 / 0xC0000409)
//   crash.log        one record with the expected kind, code, breadcrumb (and the hresult for the winrt case)
//   dumps\*.dmp      a MiniDump ("MDMP" magic), and only the newest 3 kept (access-violation case: 5 stale ones are
//                    planted first)
// Build: bash tools/build.sh crash_recorder_test      Run: build/crash_recorder_test.exe   (exit 0 = all pass)
// Never runs inside Halo: it spawns crashing children, so do not run it while the owner is using the machine.
#include <windows.h>
#include <winrt/base.h>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>
#include "../src/crash.h"

namespace {

[[noreturn]] void CrashAv() {
    HALO_CRUMB("test-stage-av");
    volatile int* p = reinterpret_cast<volatile int*>(static_cast<uintptr_t>(8));
    *p = 1;
    std::abort();  // not reached
}

// A winrt error thrown through a noexcept function: libc++ calls std::terminate, as for a throw out of a WndProc.
void ThrowsThroughNoexcept() noexcept {
    HALO_CRUMB("test-stage-terminate");
    winrt::throw_hresult(winrt::hresult(static_cast<int32_t>(0x80070057)));
}

[[noreturn]] void CrashAbort() {
    HALO_CRUMB("test-stage-abort");
    std::abort();
}

int RunChild(std::wstring const& kind, std::wstring const& dir) {
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    halo::crash::Install(dir.c_str());
    winrt::init_apartment();
    halo::crash::NoteLog(L"[halo] test log line before the crash\n");
    if (kind == L"av") CrashAv();
    if (kind == L"terminate") ThrowsThroughNoexcept();
    if (kind == L"abort") CrashAbort();
    return 99;
}

std::string ReadAll(std::wstring const& path) {
    std::string out;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) out.append(buf, n);
    CloseHandle(h);
    return out;
}

std::vector<std::wstring> Dumps(std::wstring const& dir) {
    std::vector<std::wstring> v;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((dir + L"\\dumps\\*.dmp").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return v;
    do v.push_back(fd.cFileName); while (FindNextFileW(f, &fd));
    FindClose(f);
    return v;
}

DWORD Spawn(std::wstring const& self, std::wstring const& kind, std::wstring const& dir) {
    std::wstring cmd = L"\"" + self + L"\" --child " + kind + L" \"" + dir + L"\"";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(self.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("FAIL: CreateProcess %lu\n", GetLastError());
        return 0xFFFFFFFF;
    }
    DWORD code = 0xFFFFFFFE;
    if (WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 0xFFFFFFFD);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

int failures = 0;
void Check(bool ok, char const* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

void RunCase(std::wstring const& self, wchar_t const* kind, DWORD wantExit, char const* wantKind, char const* wantCode,
             char const* wantCrumb, char const* wantExtra, bool plantStale) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"halo-crash-test-" + kind + L"-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW((dir + L"\\dumps").c_str(), nullptr);
    if (plantStale) {  // five old dumps: after the crash only the newest 3 may remain (2 stale + the new one)
        for (int i = 0; i < 5; i++) {
            std::wstring p = dir + L"\\dumps\\stale-" + std::to_wstring(i) + L".dmp";
            HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            DWORD w;
            WriteFile(h, "x", 1, &w, nullptr);
            SYSTEMTIME st{};
            st.wYear = 2020; st.wMonth = 1; st.wDay = static_cast<WORD>(1 + i);
            FILETIME ft;
            SystemTimeToFileTime(&st, &ft);
            SetFileTime(h, &ft, &ft, &ft);
            CloseHandle(h);
        }
    }
    std::printf("-- %ls\n", kind);
    DWORD code = Spawn(self, kind, dir);
    std::printf("   exit code 0x%08lX\n", code);
    Check(code == wantExit, "exit code is the original one");
    std::string log = ReadAll(dir + L"\\crash.log");
    std::printf("   crash.log: %s", log.c_str());
    Check(log.find(std::string("kind=") + wantKind) != std::string::npos, "crash.log has the expected kind");
    Check(log.find(std::string("code=0x") + wantCode) != std::string::npos, "crash.log has the exception code");
    Check(log.find(wantCrumb) != std::string::npos, "crash.log names the last breadcrumb");
    Check(log.find("test log line before the crash") != std::string::npos, "crash.log keeps the last Log() lines");
    if (wantExtra) Check(log.find(wantExtra) != std::string::npos, "crash.log has the winrt hresult");
    auto dumps = Dumps(dir);
    Check(!dumps.empty(), "a dump exists");
    bool magic = false;
    for (auto const& d : dumps) {
        if (d.rfind(L"halo-", 0) != 0) continue;
        std::string head = ReadAll(dir + L"\\dumps\\" + d);
        magic = head.size() > 1024 && head.compare(0, 4, "MDMP") == 0;
    }
    Check(magic, "the new dump is a non-trivial MDMP file");
    if (plantStale) Check(dumps.size() == 3, "only the newest 3 dumps are kept");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 4 && std::wstring(argv[1]) == L"--child") return RunChild(argv[2], argv[3]);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    RunCase(self, L"av", 0xC0000005, "unhandled-exception", "C0000005", "test-stage-av", nullptr, true);
    RunCase(self, L"terminate", 0xC0000409, "terminate", "C0000409", "test-stage-terminate", "hr=0x80070057", false);
    RunCase(self, L"abort", 0xC0000409, "abort", "C0000409", "test-stage-abort", nullptr, false);
    std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
    return failures ? 1 : 0;
}
