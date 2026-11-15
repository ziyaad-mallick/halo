// Isolated renderer for the classic Control Center and the notifications attached above it, for design checks.
//   panel_preview.exe <cc|both> <home|wifi|out|power|tab3> <out.png>
// No dock, no hooks, no guardian and nothing of the live shell: one test process with synthetic notifications
// (built with -DHALO_NOTIF_TEST), the Control Center in dry-run (toggles and sliders are logged, never performed) and,
// with HALO_TEST_CC_MEDIA=1|0, a fixed Now Playing. HALO_THEME=light|dark picks the theme; HALO_NOTIF_TEST_COUNT=n
// the number of notifications.
//
// The capture is of this process's own windows only: it puts a stand-in "desktop" (a wallpaper gradient and a fake app
// window, so glass is judged over content) in a window of its own behind the panels, and copies just the rectangle the
// panels occupy. Nothing else is on screen there while it runs.
#include <windows.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi.h>
#include <dispatcherqueue.h>
#include <wincodec.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include "../src/control_center.h"
#include "../src/notifications.h"
#include "../src/settings.h"

extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

namespace {

void Pump(int ms) {
    const auto end = GetTickCount64() + ms;
    MSG msg;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

bool g_light = false;
std::vector<uint32_t> g_wall;
SIZE g_wallSize{};

// A wallpaper and one light app window, drawn once into a DIB the backdrop window shows.
void MakeWall(int w, int h, int winL, int winT, int winR, int winB) {
    g_wallSize = {w, h};
    g_wall.assign(static_cast<size_t>(w) * h, 0);
    auto lerp = [](int a, int b, float t) { return static_cast<int>(a + (b - a) * t); };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float t = (x * 0.35f + y * 0.65f) / (w * 0.35f + h * 0.65f);
            int r, g, b;
            if (g_light) { r = lerp(0xCF, 0xF6, t); g = lerp(0xE4, 0xD9, t); b = lerp(0xF5, 0xB8, t); }
            else { r = lerp(0x17, 0x2B, t); g = lerp(0x40, 0x23, t); b = lerp(0x4A, 0x50, t); }
            g_wall[static_cast<size_t>(y) * w + x] = 0xFF000000u | r << 16 | g << 8 | b;
        }
    auto rect = [&](int l, int t, int r, int b, uint32_t c) {
        for (int y = std::max(t, 0); y < std::min(b, h); ++y)
            for (int x = std::max(l, 0); x < std::min(r, w); ++x) g_wall[static_cast<size_t>(y) * w + x] = c;
    };
    rect(winL, winT, winR, winB, 0xFFFBFBFB);
    rect(winL, winT, winR, winT + 36, 0xFFECECEC);
    for (int i = 0; i < 14; ++i) rect(winL + 28, winT + 60 + i * 26, winR - 28 - (i * 37 % 120), winT + 70 + i * 26, 0xFF9A9AA0);
    rect(winL + 28, winT + 60 + 5 * 26, winL + 160, winT + 70 + 5 * 26 + 10, 0xFFE3A322);
}

LRESULT CALLBACK WallProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), g_wallSize.cx, -g_wallSize.cy, 1, 32, BI_RGB};
        StretchDIBits(dc, 0, 0, g_wallSize.cx, g_wallSize.cy, 0, 0, g_wallSize.cx, g_wallSize.cy, g_wall.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

bool SavePng(wchar_t const* path, std::vector<uint32_t>& px, int w, int h) {
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapEncoder> enc;
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(wic.put()))) ||
        FAILED(wic->CreateStream(stream.put())) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) ||
        FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(frame.put(), nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(w, h)))
        return false;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    frame->SetPixelFormat(&fmt);
    if (FAILED(frame->WritePixels(h, w * 4, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(px.data())))) return false;
    return SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit());
}

// Copies `r` (physical px) of the screen: only the rectangle the test panels and their stand-in desktop occupy.
bool Capture(RECT r, wchar_t const* path) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    std::vector<uint32_t> px(static_cast<uint32_t*>(bits), static_cast<uint32_t*>(bits) + static_cast<size_t>(w) * h);
    for (auto& p : px) p |= 0xFF000000u;
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok && SavePng(path, px, w, h);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { std::puts("usage: panel_preview <cc|both> <home|wifi|out|power|tab3> <out.png>"); return 1; }
    const std::wstring surface = argv[1], scene = argv[2];
    // _wputenv_s, not SetEnvironmentVariableW: the panels read these with _wgetenv, which looks at the CRT's copy.
    _wputenv_s(L"HALO_TEST", L"1");  // also puts every action of the notifications in dry-run
    _wputenv_s(L"HALO_TEST_CC_DRYRUN", L"1");
    _wputenv_s(L"HALO_NOTIF_DRYRUN", L"1");
    wchar_t ini[MAX_PATH] = L"";
    if (!GetEnvironmentVariableW(L"HALO_SETTINGS_FILE", ini, MAX_PATH)) _wputenv_s(L"HALO_SETTINGS_FILE", L"nul-settings.ini");  // defaults, not the live settings
    wchar_t th[16] = L"";
    g_light = GetEnvironmentVariableW(L"HALO_THEME", th, 16) && _wcsicmp(th, L"light") == 0;
    halo::settings::Reload();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::Windows::System::DispatcherQueueController dq{nullptr};
    winrt::check_hresult(CreateDispatcherQueueController(opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(dq))));
    auto comp = winrt::Windows::UI::Composition::Compositor();
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                         D3D11_SDK_VERSION, d3d.put(), nullptr, nullptr));
    auto dxgi = d3d.as<IDXGIDevice>();
    winrt::com_ptr<ID2D1Device> d2d;
    winrt::check_hresult(D2D1CreateDevice(dxgi.get(), nullptr, d2d.put()));
    winrt::Windows::UI::Composition::CompositionGraphicsDevice gdev{nullptr};
    auto interop = comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    winrt::check_hresult(interop->CreateGraphicsDevice(reinterpret_cast<IUnknown*>(d2d.get()),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(gdev))));

    // The stand-in desktop: the right-hand strip of the work area, where the panels open.
    HINSTANCE inst = GetModuleHandleW(nullptr);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int stripW = 760, stripL = work.right - stripW, stripH = work.bottom - work.top;
    MakeWall(stripW, stripH, 60, 90, stripW - 230, stripH - 140);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = inst;
    wc.lpfnWndProc = WallProc;
    wc.lpszClassName = L"HaloPanelPreviewWall";
    RegisterClassExW(&wc);
    HWND wall = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, stripL, work.top, stripW, stripH,
                                nullptr, nullptr, inst, nullptr);
    ShowWindow(wall, SW_SHOWNOACTIVATE);
    Pump(200);

    int rc = 0;
    {
        halo::ControlCenter cc(inst, nullptr, comp, gdev);
        halo::NotificationCenter nc(inst, nullptr, comp, gdev);
        cc.siblingHwnd = [&] { return nc.Hwnd(); };
        nc.siblingHwnd = [&] { return cc.Hwnd(); };
        nc.onDismiss = [&] {};
        cc.onResized = [&] { if (nc.IsOpen()) nc.SetAnchor(cc.PanelRectScreen()); };
        Pump(300);
        cc.Open(false);
        Pump(900);
        if (surface == L"both") {
            nc.SetAnchor(cc.PanelRectScreen());
            nc.Open();
            Pump(900);
        }
        // Ids are the order of the enum in control_center.cpp: WifiText 3, OutRow 15, Power 28.
        auto click = [&](int id) { PostMessageW(cc.Hwnd(), WM_APP + 30, id, 0); };
        if (scene == L"wifi") { click(3); Pump(3500); }
        else if (scene == L"out") { click(15); Pump(1200); }
        else if (scene == L"power") { click(28); Pump(1200); }
        else if (scene == L"tab3") {
            for (int i = 0; i < 3; ++i) { PostMessageW(cc.Hwnd(), WM_KEYDOWN, VK_TAB, 0); Pump(150); }
            Pump(500);
        }
        if (surface == L"both" && nc.IsOpen()) { nc.SetAnchor(cc.PanelRectScreen()); Pump(600); }
        if (!cc.IsOpen()) { std::puts("the Control Center closed before the capture"); rc = 2; }
        else {
            RECT r = cc.PanelRectScreen();
            if (nc.IsOpen()) {  // its window is the panel plus 16 logical px of empty margin on every side
                RECT n{};
                GetWindowRect(nc.Hwnd(), &n);
                const int pad = static_cast<int>(16 * GetDpiForWindow(nc.Hwnd()) / 96.0f);
                InflateRect(&n, -pad, -pad);
                UnionRect(&r, &r, &n);
            }
            InflateRect(&r, 36, 36);  // the shadow
            RECT strip{stripL, work.top, work.right, work.bottom};
            IntersectRect(&r, &r, &strip);
            const bool ok = Capture(r, argv[3]);
            std::printf("%s %ls  rect %ld,%ld %ldx%ld\n", ok ? "captured" : "CAPTURE FAILED", argv[3], r.left, r.top, r.right - r.left, r.bottom - r.top);
            if (!ok) rc = 3;
        }
        cc.Close();
        nc.Close();
        Pump(500);
    }
    DestroyWindow(wall);
    return rc;
}
