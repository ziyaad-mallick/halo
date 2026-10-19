// Standalone harness for mods/halo-genie.wh.cpp: #includes the mod source (so it
// runs the exact same DirectComposition code), opens a 600x400 coloured window,
// animates it minimize -> restore into a fake dock icon at the bottom centre of
// the primary screen, and saves cropped PNG screenshots mid-animation.
//
//   tools/build-genie-test.ps1   (or see the clang++ line in the commit message)
//   genie_dcomp_test.exe <outdir> [ms ms ms ...]     default shots: 40 110 180 250
//
// Not part of the mod; never injects anything.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>

// Windhawk API stubs so the mod source compiles as a plain program.
static void Wh_Log(const wchar_t* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vwprintf(fmt, ap);
    va_end(ap);
    wprintf(L"\n");
}
static int Wh_SetFunctionHook(void*, void*, void**) { return 1; }
#define BOOL_WH_STUBS
#include "../mods/halo-genie.wh.cpp"

// ---- minimal PNG writer (stored deflate blocks) ----
static uint32_t Crc(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static uint32_t tab[256];
    if (!tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t v = i; for (int k = 0; k < 8; k++) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1; tab[i] = v; }
    for (size_t i = 0; i < n; i++) c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static void Chunk(FILE* f, const char* type, const std::vector<uint8_t>& data) {
    uint8_t len[4] = {(uint8_t)(data.size() >> 24), (uint8_t)(data.size() >> 16), (uint8_t)(data.size() >> 8), (uint8_t)data.size()};
    fwrite(len, 1, 4, f);
    std::vector<uint8_t> buf(type, type + 4);
    buf.insert(buf.end(), data.begin(), data.end());
    fwrite(buf.data(), 1, buf.size(), f);
    uint32_t c = ~Crc(buf.data(), buf.size());
    uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
    fwrite(cb, 1, 4, f);
}
static void WritePng(const char* path, const uint8_t* bgra, int w, int h) {
    std::vector<uint8_t> raw;
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) { const uint8_t* p = bgra + (y * w + x) * 4; raw.push_back(p[2]); raw.push_back(p[1]); raw.push_back(p[0]); }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t b : raw) { s1 = (s1 + b) % 65521; s2 = (s2 + s1) % 65521; }
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        size_t n = std::min<size_t>(65535, raw.size() - pos);
        bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(n & 0xFF); z.push_back(n >> 8); z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
        pos += n;
        if (last) break;
    }
    uint32_t ad = (s2 << 16) | s1;
    z.push_back(ad >> 24); z.push_back(ad >> 16); z.push_back(ad >> 8); z.push_back(ad);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                 (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    Chunk(f, "IHDR", ihdr);
    Chunk(f, "IDAT", z);
    Chunk(f, "IEND", {});
    fclose(f);
}

static RECT g_crop;
static std::string g_outdir;

static void Shot(const char* name) {
    int w = g_crop.right - g_crop.left, h = g_crop.bottom - g_crop.top;
    HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
    BITMAPINFO bmi = {{sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}};
    void* bits = nullptr;
    HBITMAP bm = CreateDIBSection(scr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bm);
    BitBlt(mem, 0, 0, w, h, scr, g_crop.left, g_crop.top, SRCCOPY | CAPTUREBLT);
    GdiFlush();
    WritePng((g_outdir + "/" + name).c_str(), (const uint8_t*)bits, w, h);
    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    ReleaseDC(nullptr, scr);
}

static LRESULT CALLBACK TestProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        for (int y = 0; y < r.bottom; y++) {
            HBRUSH b = CreateSolidBrush(RGB(40 + y * 120 / r.bottom, 90, 220 - y * 120 / r.bottom));
            RECT row = {0, y, r.right, y + 1};
            FillRect(dc, &row, b);
            DeleteObject(b);
        }
        HBRUSH title = CreateSolidBrush(RGB(250, 190, 30));
        RECT t = {0, 0, r.right, 48};
        FillRect(dc, &t, title);
        DeleteObject(title);
        HBRUSH box = CreateSolidBrush(RGB(240, 60, 80));
        RECT bx = {40, 90, 240, 230};
        FillRect(dc, &bx, box);
        DeleteObject(box);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        HFONT f = CreateFontW(48, 0, 0, 0, FW_BOLD, 0, 0, 0, 0, 0, 0, 0, 0, L"Segoe UI");
        SelectObject(dc, f);
        TextOutW(dc, 270, 120, L"Halo genie", 10);
        TextOutW(dc, 270, 190, L"DComp test", 10);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

static void Pump(int ms) {
    DWORD end = GetTickCount() + ms;
    while ((int)(end - GetTickCount()) > 0) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(2);
    }
}

struct ShotPlan { std::vector<int> ms; std::string prefix; LARGE_INTEGER t0; };
static DWORD WINAPI ShotThread(LPVOID p) {
    auto* sp = (ShotPlan*)p;
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    for (size_t i = 0; i < sp->ms.size(); i++) {
        for (;;) {
            QueryPerformanceCounter(&now);
            if ((now.QuadPart - sp->t0.QuadPart) * 1000 / f.QuadPart >= sp->ms[i]) break;
            Sleep(1);
        }
        char name[64];
        snprintf(name, sizeof(name), "%s_%03dms.png", sp->prefix.c_str(), sp->ms[i]);
        Shot(name);
        {   // what the mouse would hit mid-animation: the ghost must be click-through
            POINT pt = {460, 300};  // the test window's centre: covered by the ghost while it plays
            HWND under = WindowFromPoint(pt);
            wchar_t cls[64] = {}; GetClassNameW(under, cls, 64);
            DWORD pid = 0; GetWindowThreadProcessId(under, &pid);
            printf("  under window centre: %ls (own process: %d)\n", cls, (int)(pid == GetCurrentProcessId()));
        }
        QueryPerformanceCounter(&now);
        printf("%s taken at %.0f ms\n", name, (now.QuadPart - sp->t0.QuadPart) * 1000.0 / f.QuadPart);
    }
    return 0;
}

static void Play(HWND h, Kind kind, const std::vector<int>& shots, const char* prefix, Geometry icon) {
    auto a = std::make_shared<Anim>();
    a->hwnd = h; a->kind = kind; a->icon = icon;
    g_active.insert(h);
    SetTransitions(h, FALSE);
    bool ok;
    if (kind == Restore) {
        a->cloaked = true;
        Cloak(h, TRUE);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);  // a real app repaints itself on restore
        ok =Prepare(a) && Launch(a, false);
    } else {
        LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
        ok = Prepare(a);
        LARGE_INTEGER tp; QueryPerformanceCounter(&tp);
        printf("%s: prepare (capture) %.1f ms\n", prefix, (tp.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        ok = ok && Launch(a, true);
        QueryPerformanceCounter(&t1);
        printf("%s: caller stall %.1f ms\n", prefix, (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart);
        if (ok) ShowWindow(h, SW_HIDE);  // the "real" minimize
    }
    printf("%s: launched=%d\n", prefix, (int)ok);
    ShotPlan sp{shots, prefix, {}};
    QueryPerformanceCounter(&sp.t0);
    HANDLE t = CreateThread(nullptr, 0, ShotThread, &sp, 0, nullptr);
    while (g_workers.load() > 0) Pump(5);
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
    Pump(150);
}

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_outdir = argc > 1 ? argv[1] : ".";
    std::vector<int> shots;
    for (int i = 2; i < argc; i++) shots.push_back(atoi(argv[i]));
    if (shots.empty()) shots = {40, 110, 180, 250};

    WNDCLASSW wc = {};
    wc.lpfnWndProc = TestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"GenieTestWin";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"GenieTestWin", L"genie test", WS_POPUP | WS_VISIBLE, 160, 100, 600, 400, nullptr, nullptr, wc.hInstance, nullptr);
    Pump(400);

    float cx = sw / 2.0f, cy = sh - 40.0f;
    Geometry icon = {cx - 24, cy - 24, 48, 48};
    g_crop = {60, 40, std::min(sw, sw / 2 + 300), sh};
    printf("screen %dx%d icon centre (%.0f,%.0f) crop %ld,%ld-%ld,%ld\n", sw, sh, cx, cy, g_crop.left, g_crop.top, g_crop.right, g_crop.bottom);
    if (GetEnvironmentVariableW(L"GENIE_PREWARM", nullptr, 0)) {  // as if the pointer had crossed the minimize button
        Prewarm();
        Pump(400);
    }
    Shot("00_before.png");
    Play(h, Minimize, shots, "minimize", icon);
    Shot("01_minimized.png");
    Pump(300);
    Play(h, Restore, shots, "restore", icon);
    Pump(300);
    Play(h, Minimize, {}, "minimize-again", icon);  // second time: the D3D device already exists
    Pump(300);
    Play(h, Restore, {}, "restore-again", icon);
    { DWORD c = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c));
      printf("after restore: visible=%d cloaked=%lu iconic=%d\n", (int)IsWindowVisible(h), c, (int)IsIconic(h)); }
    Shot("02_restored.png");
    Pump(800);
    Shot("03_restored_later.png");
    RECT wr; GetWindowRect(h, &wr);
    printf("window rect %ld,%ld-%ld,%ld topmost=%d\n", wr.left, wr.top, wr.right, wr.bottom, (int)!!(GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST));
    DestroyWindow(h);
    return 0;
}
