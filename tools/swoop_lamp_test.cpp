#include <windows.h>
#include <cstdio>
#include <string>
#include "../src/swoop.h"

static LRESULT CALLBACK TestProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void Pump(int ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
        Sleep(5);
    }
}
int wmain(int argc, wchar_t**) {
    if (argc > 1) {
        WNDCLASSW wc{}; wc.lpfnWndProc = TestProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"HaloLampChild";
        RegisterClassW(&wc);
        CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Halo lamp test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                        200, 100, 500, 400, nullptr, nullptr, wc.hInstance, nullptr);
        MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        return 0;
    }
    fprintf(stderr, "starting lamp smoke test\n");
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + path + L"\" child";
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return 1;
    HWND child = nullptr;
    for (int i = 0; i < 100 && !child; ++i) { Pump(20); child = FindWindowW(L"HaloLampChild", nullptr); }
    if (!child) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return 2; }
    int fail = 0;
    {
        halo::Swoop s([](HWND, RECT const&, RECT& icon) { icon = {400, 700, 448, 748}; return true; }, true);
        s.Track(child);
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(60);
        bool minimizing = FindWindowW(L"HaloSwoop", nullptr) != nullptr;
        printf("lamp overlay on minimize: %s\n", minimizing ? "yes" : "no");
        if (!minimizing) ++fail;
        Pump(500);
        bool restored = false;
        bool started = s.Restore(child, [&] { restored = true; ShowWindowAsync(child, SW_RESTORE); });
        Pump(80);
        bool overlay = FindWindowW(L"HaloSwoop", nullptr) != nullptr;
        Pump(600);
        printf("lamp restore started=%d overlay=%d callback=%d iconic=%d\n", started, overlay, restored, IsIconic(child));
        if (!started || !overlay || !restored || IsIconic(child)) ++fail;
        if (FindWindowW(L"HaloSwoop", nullptr)) ++fail;
        s.SetEnabled(false);
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(100);
        bool suppressed = !FindWindowW(L"HaloSwoop", nullptr) && !s.Restore(child, [] {});
        printf("reduced motion suppresses lamp: %s\n", suppressed ? "yes" : "no");
        if (!suppressed) ++fail;
    }
    PostMessageW(child, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    printf("lamp smoke failures: %d\n", fail);
    fflush(stdout);
    return fail ? 1 : 0;
}
