// Samples a patch of the composed screen across a dock restore of a synthetic red/blue window.
// A blink = the patch stops being solid red after the ghost already covered it. Prints a timeline.
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/swoop.h"

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
        HBRUSH a = CreateSolidBrush(RGB(240, 40, 20));
        FillRect(dc, &r, a); DeleteObject(a); EndPaint(h, &ps); return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}
static void Pump() { MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg); }
static double Red(HDC screen, HDC mem, unsigned* bits, int x, int y, int n) {
    BitBlt(mem, 0, 0, n, n, screen, x, y, SRCCOPY); GdiFlush();
    int hit = 0;
    for (int i = 0; i < n*n; ++i) { unsigned p = bits[i] & 0xffffff; int r = p>>16&255, g = p>>8&255, b = p&255;
        if (abs(r-240) < 6 && abs(g-40) < 6 && abs(b-20) < 6) ++hit; }
    return hit / double(n*n);
}
int wmain(int argc, wchar_t**) {
    if (argc > 1) {
        WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"HaloProbeChild";
        RegisterClassW(&wc);
        CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 200, 100, 500, 400, nullptr, nullptr, wc.hInstance, nullptr);
        MSG msg; while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
        return 0;
    }
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + path + L"\" child";
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return 1;
    HWND child = nullptr;
    for (int i = 0; i < 100 && !child; ++i) { Sleep(20); Pump(); child = FindWindowW(L"HaloProbeChild", nullptr); }
    if (!child) return 2;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    const int N = 24;
    BITMAPINFO bmi{}; bmi.bmiHeader = {sizeof(BITMAPINFOHEADER), N, -N, 1, 32, BI_RGB};
    void* bits = nullptr; HBITMAP bm = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0); SelectObject(mem, bm);
    int patchX = 300, patchY = 220;  // inside the client area of the 500x400 window at (200,100)
    LARGE_INTEGER f, t0; QueryPerformanceFrequency(&f);
    auto ms = [&] { LARGE_INTEGER n; QueryPerformanceCounter(&n); return (n.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart; };
    int rounds = 8;
    for (int round = 0; round < rounds; ++round) {
        halo::Swoop s([](HWND, RECT const&, RECT& icon) { icon = {400, 700, 448, 748}; return true; }, true);
        s.Track(child);
        for (int i = 0; i < 20; ++i) { Sleep(10); Pump(); }
        QueryPerformanceCounter(&t0);
        SendMessageTimeoutW(child, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 500, nullptr);
        { int gaps = 0; double firstGap = -1, last = 0;
          while (ms() < 900) { Pump(); double t = ms(), r = Red(screen, mem, (unsigned*)bits, patchX, patchY, N);
            if (t < 120 && r < .98) { ++gaps; if (firstGap < 0) firstGap = t; last = t; } }
          printf("round %d minimize: non-red samples in first 120 ms = %d (first at %.0f, last at %.0f)\n", round, gaps, firstGap, last); }
        std::vector<std::pair<double,double>> samples; double actedAt = -1;
        QueryPerformanceCounter(&t0);
        s.Restore(child, [&] { actedAt = ms(); ShowWindowAsync(child, SW_RESTORE); });
        double firstFull = -1, worstAfter = 1, lastBad = -1;
        while (ms() < 900) {
            Pump();
            double t = ms(), r = Red(screen, mem, (unsigned*)bits, patchX, patchY, N);
            samples.push_back({t, r});
            if (firstFull < 0 && r > .98 && t > 300) firstFull = t;
            if (firstFull >= 0 && t > firstFull) { worstAfter = std::min(worstAfter, r); if (r < .98) lastBad = t; }
        }
        // Dropout run: consecutive samples after first-full that are not solid red.
        int bad = 0; for (auto& s2 : samples) if (firstFull >= 0 && s2.first > firstFull && s2.second < .98) ++bad;
        printf("round %d: acted=%.0f ms firstFull=%.0f ms samples=%zu dropouts-after-full=%d worst=%.2f lastBad=%.0f\n",
               round, actedAt, firstFull, samples.size(), bad, worstAfter, lastBad);
        RECT home{}; GetWindowRect(child, &home);
        printf("  final rect %ld,%ld %ldx%ld iconic=%d\n", home.left, home.top, home.right-home.left, home.bottom-home.top, IsIconic(child));
        if (round == rounds-1 || bad)
            for (auto& s2 : samples) if (s2.first > (firstFull > 0 ? firstFull - 30 : 400) && s2.first < (firstFull > 0 ? firstFull + 140 : 900))
                printf("  t=%6.1f red=%.2f\n", s2.first, s2.second);
        for (int i = 0; i < 20; ++i) { Sleep(10); Pump(); }
    }
    PostMessageW(child, WM_CLOSE, 0, 0); WaitForSingleObject(pi.hProcess, 3000);
    return 0;
}
