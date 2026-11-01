#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cwchar>

static BOOL CALLBACK FindGhost(HWND h, LPARAM data) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64]{}; GetClassNameW(h, cls, 64);
    if (pid == GetCurrentProcessId() && !wcscmp(cls, L"Static") &&
        (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_NOREDIRECTIONBITMAP) && IsWindowVisible(h))
        *reinterpret_cast<bool*>(data) = true;
    return TRUE;
}

static void Pump(DWORD ms) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(5);
    }
}

static bool HasGenie() {
    HMODULE modules[1024]; DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return false;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i) {
        wchar_t name[MAX_PATH]{};
        if (GetModuleBaseNameW(GetCurrentProcess(), modules[i], name, MAX_PATH) && wcsstr(name, L"halo-genie_2.2.1")) return true;
    }
    return false;
}

int main() {
    for (int i = 0; i < 30 && !HasGenie(); ++i) Sleep(100);
    bool loaded = HasGenie();
    printf("genie mod loaded in test process: %s\n", loaded ? "yes" : "no");
    if (!loaded) return 1;
    WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"HaloInjectionTest";
    if (!RegisterClassW(&wc)) return 2;
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Halo genie test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             160, 100, 600, 400, nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) return 3;
    Pump(2000);
    ShowWindow(h, SW_MINIMIZE);
    bool ghost = false;
    for (int i = 0; i < 40 && !ghost; ++i) { EnumWindows(FindGhost, reinterpret_cast<LPARAM>(&ghost)); Pump(5); }
    printf("genie ghost after minimize: %s\n", ghost ? "yes" : "no");
    Pump(600);
    ShowWindow(h, SW_RESTORE);
    bool restoring = false;
    for (int i = 0; i < 60 && !restoring; ++i) { EnumWindows(FindGhost, reinterpret_cast<LPARAM>(&restoring)); Pump(5); }
    printf("genie ghost after restore: %s\n", restoring ? "yes" : "no");
    Pump(600);
    DestroyWindow(h);
    return ghost && restoring ? 0 : 4;
}
