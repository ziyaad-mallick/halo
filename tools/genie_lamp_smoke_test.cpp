#define main genie_visual_harness_main
#include "genie_dcomp_test.cpp"
#undef main

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSW wc{};
    wc.lpfnWndProc = TestProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HaloLampSmokeTest";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"Lamp test",
                             WS_POPUP | WS_VISIBLE, 160, 100, 600, 400, nullptr, nullptr, wc.hInstance, nullptr);
    if (!h) return 1;
    Pump(200);
    Geometry icon{(float)(GetSystemMetrics(SM_CXSCREEN) / 2), (float)(GetSystemMetrics(SM_CYSCREEN) - 60), 48, 48};
    int failures = 0;
    for (Kind kind : {Minimize, Restore}) {
        if (kind == Restore) ShowWindow(h, SW_SHOWNOACTIVATE);
        auto a = std::make_shared<Anim>();
        a->hwnd = h;
        a->kind = kind;
        g_active.insert(h);
        a->icon = icon;
        bool ok = Prepare(a) && Launch(a, true);
        if (!ok) { Abandon(a); ++failures; break; }
        if (kind == Minimize) ShowWindow(h, SW_HIDE);
        while (g_workers.load()) Pump(5);
        if (g_active.count(h)) ++failures;
    }
    DestroyWindow(h);
    ReleaseDxgiDevice();
    printf("lamp smoke failures: %d\n", failures);
    return failures ? 1 : 0;
}
