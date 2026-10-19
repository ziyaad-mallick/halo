// Self-test for src/swoop.cpp against a window of its own: this exe spawns itself with --child, which shows a
// gradient window; the parent minimizes and restores it through Swoop and watches the overlay.
// (Swoop's hook skips the process's own windows, hence the child.) Run: build/swoop_test.exe
#include <windows.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string>

#include "../src/swoop.h"

static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        for (int x = 0; x < r.right; x += 8) {
            HBRUSH b = CreateSolidBrush(RGB(x * 255 / r.right, 40 + (x * 7) % 180, 255 - x * 255 / r.right));
            RECT c{x, 0, x + 8, r.bottom};
            FillRect(dc, &c, b);
            DeleteObject(b);
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(h, m, w, l);
}

static int Child() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = ChildProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SwoopTestChild";
    RegisterClassW(&wc);
    CreateWindowExW(0, L"SwoopTestChild", L"SwoopTestChild", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 150, 500, 400, nullptr,
                    nullptr, wc.hInstance, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    return 0;
}

static double Variance(RECT r) {
    int w = r.right - r.left, hgt = r.bottom - r.top;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi = {{sizeof(BITMAPINFOHEADER), w, -hgt, 1, 32, BI_RGB}};
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, bm);
    BitBlt(mem, 0, 0, w, hgt, scr, r.left, r.top, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    auto* p = static_cast<unsigned*>(bits);
    double s = 0, s2 = 0;
    int n = w * hgt;
    for (int i = 0; i < n; i++) {
        double v = (p[i] & 0xFF) + ((p[i] >> 8) & 0xFF) + ((p[i] >> 16) & 0xFF);
        s += v;
        s2 += v * v;
    }
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
    double m = s / n;
    return s2 / n - m * m;
}

static int fails = 0;
static void Check(bool ok, char const* what) {
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}

static void Pump(int ms, HWND* overlay = nullptr, int* seen = nullptr, double* maxVar = nullptr) {
    ULONGLONG end = GetTickCount64() + ms;
    int n = 0;
    while (GetTickCount64() < end) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        if (overlay) {
            HWND o = FindWindowW(L"HaloSwoop", nullptr);
            if (o && seen) (*seen)++;
            if (o && maxVar && ++n % 4 == 0 && IsWindowVisible(o)) {
                RECT r;
                GetWindowRect(o, &r);
                InflateRect(&r, -(r.right - r.left) / 3, -(r.bottom - r.top) / 3);
                *maxVar = std::max(*maxVar, Variance(r));
            }
            if (o) *overlay = o;
        }
        Sleep(4);
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && !wcscmp(argv[1], L"--child")) return Child();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + self + L"\" --child";
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return 1;
    HWND child = nullptr;
    for (int i = 0; i < 100 && !child; i++) {
        Sleep(30);
        child = FindWindowW(L"SwoopTestChild", nullptr);
    }
    Check(child != nullptr, "child window appeared");
    if (!child) { TerminateProcess(pi.hProcess, 0); return 1; }
    Sleep(300);

    int iconCalls = 0;
    RECT iconRect{300, 1000, 340, 1040};
    {
        halo::Swoop sw([&](HWND, RECT const&, RECT& icon) { iconCalls++; icon = iconRect; return true; }, true);
        sw.Track(child);
        // (DWMWA_TRANSITIONS_FORCEDISABLED can't be read back, so the checks below are on the overlay's behaviour;
        // a window that wasn't tracked would show no overlay at all.)

        HWND ov = nullptr;
        int seen = 0;
        double var = 0;
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(60, &ov, &seen, &var);  // mid-animation
        Check(seen > 0 && ov != nullptr, "overlay exists while minimizing");
        if (ov) {
            LONG_PTR ex = GetWindowLongPtrW(ov, GWL_EXSTYLE);
            constexpr LONG_PTR want = WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
            Check((ex & want) == want, "overlay is click-through, no-activate, topmost");
            Check(GetForegroundWindow() != ov, "overlay did not take focus");
        }
        Pump(500, &ov, &seen, &var);
        Check(IsIconic(child), "window is minimized");
        Check(FindWindowW(L"HaloSwoop", nullptr) == nullptr, "overlay destroyed after minimize");
        printf("  overlay pixel variance seen: %.0f (UNVERIFIED visually if 0)\n", var);
        Check(var > 100, "the thumbnail drew content into the overlay");

        bool restored = false;
        ov = nullptr;
        seen = 0;
        var = 0;
        ULONGLONG t0 = GetTickCount64();
        bool started = sw.Restore(child, [&] {
            restored = true;
            SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        });
        Check(started, "Restore reports a swoop");
        ULONGLONG actedAt = 0;
        while (GetTickCount64() - t0 < 900) {
            Pump(10, &ov, &seen, &var);
            if (restored && !actedAt) actedAt = GetTickCount64() - t0;
        }
        printf("  restore action ran at %llu ms\n", actedAt);
        Check(restored && actedAt < 300, "restore ran within 300 ms");
        Check(!IsIconic(child), "window is restored");
        Check(seen > 0, "overlay exists while restoring");
        Check(FindWindowW(L"HaloSwoop", nullptr) == nullptr, "overlay destroyed after restore");

        // a minimize that the genie mod claimed is left alone
        sw.ModAsked(child);
        seen = 0;
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(200, &ov, &seen, nullptr);
        Check(seen == 0, "no overlay when the mod asked");
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(300);

        // a destroyed-mid-animation window must not leave the overlay behind
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(60);
        sw.Forget(child);
        Check(FindWindowW(L"HaloSwoop", nullptr) == nullptr, "Forget ends a running swoop");
        Pump(300);
        sw.Track(child);
        sw.SetEnabled(false);  // reduce_motion: no swoop
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(300);
        seen = 0;
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(200, &ov, &seen, nullptr);
        Check(seen == 0, "no overlay under reduce_motion");
        Check(!sw.Restore(child, [] {}), "Restore declines under reduce_motion");
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        Pump(300);
        Check(iconCalls > 0, "icon callback used");
    }
    TerminateProcess(pi.hProcess, 0);
    printf("%d failure(s)\n", fails);
    return fails ? 1 : 0;
}
