// taskbar_probe.exe - PASSIVE observer of the real Windows taskbar. It never calls ShowWindow,
// SetWindowPos, SHAppBarMessage(SETSTATE) or anything else that changes a window: it only listens
// (WinEvent hooks, shell-hook messages, broadcasts) and reads state, to learn what makes Explorer
// show an already-hidden taskbar. Usage: taskbar_probe.exe <seconds> <logfile>
#include <windows.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

FILE* g_log = nullptr;
DWORD g_explorerPid = 0;

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[900];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(msg, 899, fmt, ap);
    va_end(ap);
    msg[899] = 0;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1000];
    int n = _snwprintf(line, 999, L"%02d:%02d:%02d.%03d  %ls\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    if (n <= 0 || !g_log) return;
    char u8[3000];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, n, u8, sizeof(u8), nullptr, nullptr);
    fwrite(u8, 1, static_cast<size_t>(len), g_log);
    fflush(g_log);
}

std::wstring Class(HWND h) {
    wchar_t c[96] = {};
    GetClassNameW(h, c, 95);
    return c;
}

bool IsTaskbarClass(const std::wstring& c) { return c == L"Shell_TrayWnd" || c == L"Shell_SecondaryTrayWnd"; }

std::wstring ExeOf(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"?";
    wchar_t b[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    std::wstring r = QueryFullProcessImageNameW(h, 0, b, &n) ? b : L"?";
    CloseHandle(h);
    size_t s = r.find_last_of(L'\\');
    return s == std::wstring::npos ? r : r.substr(s + 1);
}

// Everything that could explain a taskbar show, sampled at the moment it happens.
std::wstring Context() {
    POINT p{};
    GetCursorPos(&p);
    HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(m, &mi);
    HWND fg = GetForegroundWindow();
    DWORD fpid = 0;
    if (fg) GetWindowThreadProcessId(fg, &fpid);
    QUERY_USER_NOTIFICATION_STATE q{};
    SHQueryUserNotificationState(&q);
    wchar_t b[600];
    _snwprintf(b, 599, L"cur=(%ld,%ld) toMonBottom=%ld fg=%ls/%ls qun=%d lwin=%d rwin=%d", p.x, p.y, mi.rcMonitor.bottom - p.y,
               fg ? Class(fg).c_str() : L"-", fg ? ExeOf(fpid).c_str() : L"-", static_cast<int>(q),
               (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0, (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0);
    return b;
}

std::wstring Describe(HWND h) {
    RECT r{};
    GetWindowRect(h, &r);
    HRGN rg = CreateRectRgn(0, 0, 0, 0);
    int rt = GetWindowRgn(h, rg);
    DeleteObject(rg);
    BYTE a = 255;
    DWORD fl = 0;
    bool lay = (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_LAYERED) != 0;
    if (lay) GetLayeredWindowAttributes(h, nullptr, &a, &fl);
    wchar_t b[300];
    _snwprintf(b, 299, L"hwnd=%p vis=%d rect=(%ld,%ld,%ld,%ld) style=%08lX ex=%08lX rgn=%d layered=%d alpha=%d", h, IsWindowVisible(h),
               r.left, r.top, r.right, r.bottom, static_cast<DWORD>(GetWindowLongW(h, GWL_STYLE)),
               static_cast<DWORD>(GetWindowLongW(h, GWL_EXSTYLE)), rt, lay, a);
    return b;
}

void Snapshot(const wchar_t* why) {
    APPBARDATA abd{sizeof(abd)};
    UINT st = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    Log(L"SNAP %ls appbarState=%u workarea=(%ld,%ld,%ld,%ld)", why, st, wa.left, wa.top, wa.right, wa.bottom);
    HWND p = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (p) Log(L"SNAP   primary %ls", Describe(p).c_str());
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) Log(L"SNAP   secondary %ls", Describe(h).c_str());
    if (p) {
        EnumChildWindows(p, [](HWND c, LPARAM) -> BOOL {
            if (GetParent(c) == FindWindowW(L"Shell_TrayWnd", nullptr)) Log(L"SNAP   child %ls %ls", Class(c).c_str(), Describe(c).c_str());
            return TRUE;
        }, 0);
    }
}

void CALLBACK OnTaskbarEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    std::wstring c = Class(h);
    if (!IsTaskbarClass(c)) {
        if (ev == EVENT_OBJECT_SHOW || ev == EVENT_OBJECT_HIDE)
            if (GetAncestor(h, GA_ROOT) == h) Log(L"EXPLORER-WND %ls %ls", ev == EVENT_OBJECT_SHOW ? L"SHOW" : L"HIDE", c.c_str());
        return;
    }
    const wchar_t* n = ev == EVENT_OBJECT_SHOW ? L"SHOW" : ev == EVENT_OBJECT_HIDE ? L"HIDE" : L"LOCCHG";
    Log(L"TB %ls %ls %ls | %ls", n, c.c_str(), Describe(h).c_str(), Context().c_str());
}

// Top-level windows of any other process appearing: toasts, flyouts, Start.
void CALLBACK OnAnyShow(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h || GetAncestor(h, GA_ROOT) != h) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_explorerPid || pid == GetCurrentProcessId()) return;
    std::wstring c = Class(h);
    if (c == L"tooltips_class32" || c == L"IME" || c == L"MSCTFIME UI") return;
    Log(L"APP-WND SHOW %ls (%ls)", c.c_str(), ExeOf(pid).c_str());
}

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND h, LONG, LONG, DWORD, DWORD) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    Log(L"FOREGROUND %ls (%ls)", Class(h).c_str(), ExeOf(pid).c_str());
}

UINT g_taskbarCreated = 0, g_shellHook = 0;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == g_taskbarCreated) {
        Log(L"BROADCAST TaskbarCreated");
        return 0;
    }
    if (m == g_shellHook) {
        Log(L"SHELLHOOK code=0x%llX lparam=%p", static_cast<unsigned long long>(w), reinterpret_cast<void*>(l));
        return 0;
    }
    switch (m) {
        case WM_DISPLAYCHANGE: Log(L"BROADCAST WM_DISPLAYCHANGE %lux%lu", LOWORD(l), HIWORD(l)); return 0;
        case WM_DPICHANGED: Log(L"BROADCAST WM_DPICHANGED"); return 0;
        case WM_SETTINGCHANGE:
            Log(L"BROADCAST WM_SETTINGCHANGE wp=%llu %ls", static_cast<unsigned long long>(w), l ? reinterpret_cast<const wchar_t*>(l) : L"-");
            return 0;
        case WM_POWERBROADCAST: Log(L"BROADCAST WM_POWERBROADCAST %llu", static_cast<unsigned long long>(w)); return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    int seconds = argc > 1 ? _wtoi(argv[1]) : 300;
    const wchar_t* path = argc > 2 ? argv[2] : L"taskbar-probe.log";
    g_log = _wfopen(path, L"ab");
    if (!g_log) return 1;
    if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) GetWindowThreadProcessId(tray, &g_explorerPid);
    Log(L"probe start, %d s, explorer pid %lu", seconds, g_explorerPid);
    Snapshot(L"start");

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HaloTaskbarProbe";
    RegisterClassW(&wc);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_shellHook = RegisterWindowMessageW(L"SHELLHOOK");
    HWND w = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"probe", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    RegisterShellHookWindow(w);

    HWINEVENTHOOK h1 = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_HIDE, nullptr, OnTaskbarEvent, g_explorerPid, 0, WINEVENT_OUTOFCONTEXT);
    HWINEVENTHOOK h2 = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnTaskbarEvent, g_explorerPid, 0, WINEVENT_OUTOFCONTEXT);
    HWINEVENTHOOK h3 = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnAnyShow, 0, 0, WINEVENT_OUTOFCONTEXT);
    HWINEVENTHOOK h4 = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
    Log(L"hooks %p %p %p %p", h1, h2, h3, h4);

    const DWORD end = GetTickCount() + static_cast<DWORD>(seconds) * 1000;
    while (true) {
        DWORD now = GetTickCount();
        LONG left = static_cast<LONG>(end - now);
        if (left <= 0) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(left), QS_ALLINPUT);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    Snapshot(L"end");
    for (HWINEVENTHOOK h : {h1, h2, h3, h4}) if (h) UnhookWinEvent(h);
    DeregisterShellHookWindow(w);
    Log(L"probe end");
    fclose(g_log);
    return 0;
}
