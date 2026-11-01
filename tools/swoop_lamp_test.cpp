#include <windows.h>
#include <cstdio>
#include <string>
#include "../src/swoop.h"

static LRESULT CALLBACK TestProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT r; GetClientRect(h, &r);
        HBRUSH left = CreateSolidBrush(RGB(240, 40, 20)), right = CreateSolidBrush(RGB(20, 60, 240));
        RECT half{0, 0, r.right / 2, r.bottom}; FillRect(dc, &half, left);
        half.left = r.right / 2; half.right = r.right; FillRect(dc, &half, right);
        DeleteObject(left); DeleteObject(right); EndPaint(h, &ps); return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void ProbeCapture(HWND h, const char* stage) {
    RECT r{}; GetWindowRect(h, &r);
    int w = r.right-r.left, hgt = r.bottom-r.top;
    if (w <= 0 || hgt <= 0 || w > 10000 || hgt > 10000) { printf("capture %s: invalid size\n", stage); return; }
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bmi{}; bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -hgt; bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32;
    void* bits = nullptr; HBITMAP bm = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bm) { DeleteDC(dc); return; }
    HGDIOBJ old = SelectObject(dc, bm); auto t = GetTickCount64();
    BOOL ok = PrintWindow(h, dc, 2); GdiFlush(); auto elapsed = GetTickCount64()-t;
    auto pixels = static_cast<unsigned*>(bits); int red = 0, blue = 0;
    for (int i = 0; i < w*hgt; ++i) { unsigned p = pixels[i]; if ((p&0xffffff) == 0xf02814) ++red; if ((p&0xffffff) == 0x143cf0) ++blue; }
    printf("capture %s: ok=%d red=%d blue=%d elapsed=%llu ms\n", stage, ok, red, blue, elapsed);
    SelectObject(dc, old); DeleteObject(bm); DeleteDC(dc);
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
        Pump(200);
#ifdef HALO_SWOOP_TEST_STALL
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(35);
        ULONGLONG closeAt = GetTickCount64();
        s.Forget(child);
        ULONGLONG closeMs = GetTickCount64() - closeAt;
        printf("stalled renderer cleanup returned in %llu ms\n", closeMs);
        if (closeMs > 120 || FindWindowW(L"HaloSwoop", nullptr)) ++fail;
        Pump(300);
#else
        ProbeCapture(child, "visible");
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(60);
        bool minimizing = FindWindowW(L"HaloSwoop", nullptr) != nullptr;
        printf("lamp overlay on minimize: %s\n", minimizing ? "yes" : "no");
        if (!minimizing) ++fail;
#ifdef HALO_SWOOP_PROFILE
        Sleep(200);  // Simulate a busy dock thread; the renderer must keep advancing.
#endif
        Pump(500);
        ProbeCapture(child, "minimized");
        bool restored = false;
        ULONGLONG startedAt = GetTickCount64(), restoredAt = 0;
        bool started = s.Restore(child, [&] { restored = true; restoredAt = GetTickCount64() - startedAt; ShowWindowAsync(child, SW_RESTORE); });
        Pump(80);
        bool overlay = FindWindowW(L"HaloSwoop", nullptr) != nullptr;
        Pump(600);
        printf("lamp restore started=%d overlay=%d callback=%d at=%llu ms iconic=%d\n", started, overlay, restored, restoredAt, IsIconic(child));
        if (!started || !overlay || !restored || restoredAt < 350 || restoredAt > 650 || IsIconic(child)) ++fail;
        if (FindWindowW(L"HaloSwoop", nullptr)) ++fail;
        s.SetEnabled(false);
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(100);
        bool suppressed = !FindWindowW(L"HaloSwoop", nullptr) && !s.Restore(child, [] {});
        printf("reduced motion suppresses lamp: %s\n", suppressed ? "yes" : "no");
        if (!suppressed) ++fail;
        ShowWindowAsync(child, SW_RESTORE); Pump(150); s.SetEnabled(true);
        int interruptionFailures = 0;
        for (int i = 0; i < 12; ++i) {
            SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
            Pump(25);
            s.Forget(child);
            if (FindWindowW(L"HaloSwoop", nullptr)) ++interruptionFailures;
            ShowWindowAsync(child, SW_RESTORE); Pump(75);
            s.Track(child);
        }
        printf("interrupted animation cleanup failures: %d\n", interruptionFailures);
        fail += interruptionFailures;
#endif
    }
    PostMessageW(child, WM_CLOSE, 0, 0);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    printf("lamp smoke failures: %d\n", fail);
    fflush(stdout);
    return fail ? 1 : 0;
}
