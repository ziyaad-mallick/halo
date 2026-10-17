// tools/thumbtest.cpp — standalone DWM-thumbnail checks for Halo's hover previews (task 3a).
//
// Self-contained: the process creates its own demo content windows (no external
// apps are launched), previews them with DwmRegisterThumbnail into the
// candidate destination-window styles, saves screenshots of what the compositor
// actually drew, and counts handles across register/unregister cycles. It never
// touches the running dock and it exits by itself.
//
//   usage: thumbtest.exe <output-dir>
//
// Variants under test (all destinations are topmost tool windows like the dock's):
//   A: plain WS_POPUP + DwmExtendFrameIntoClientArea(-1)    (aero-peek style glass)
//   C: plain WS_POPUP + WS_EX_LAYERED + UpdateLayeredWindow (per-pixel alpha bg)
//   D: WS_EX_NOREDIRECTIONBITMAP + Windows.UI.Composition   (the dock's own stack)
//   F: plain WS_POPUP + rounded SetWindowRgn                (does a region clip thumbs?)
// Plus: registration on a hidden destination, a registered-then-destroyed source,
// a minimized source, and a 300x register/unregister cycle handle-count check.

#include <windows.h>
#include <initguid.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <shellscalingapi.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1_1.h>
#include <dispatcherqueue.h>
#include <windows.ui.composition.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002  // Win 8.1+: include DWM-composited content
#endif

namespace wuc = winrt::Windows::UI::Composition;

FILE* g_log = nullptr;

void Logf(wchar_t const* fmt, ...) {
    wchar_t line[512];
    va_list args;
    va_start(args, fmt);
    vswprintf(line, 512, fmt, args);
    va_end(args);
    OutputDebugStringW(line);
    fputws(line, stderr);
    if (g_log) { fputws(line, g_log); fflush(g_log); }
}

char const* HrName(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case E_ACCESSDENIED: return "E_ACCESSDENIED";
        case DWM_E_COMPOSITIONDISABLED: return "DWM_E_COMPOSITIONDISABLED";
        case DWM_E_SOURCE_NOT_REGISTERED: return "DWM_E_SOURCE_NOT_REGISTERED";
        case E_INVALIDARG: return "E_INVALIDARG";
        default: return "";
    }
}

// Interop interfaces missing from the MinGW SDK headers (as in src/halo.cpp).
struct ICompositorDesktopInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopWindowTarget(HWND hwnd, BOOL isTopmost, void** target) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnsureOnThread(DWORD threadId) = 0;
};
__CRT_UUID_DECL(ICompositorDesktopInterop, 0x29E691FA, 0x4567, 0x4DCA, 0xB3, 0x19, 0xD0, 0xF2, 0x07, 0xEB, 0x68, 0x07)

// ----------------------------- demo content windows -------------------------
// Two borderless windows with a moving strip and a per-400ms counter so two
// screenshots can prove the thumbnails live-update.

struct Demo {
    wchar_t tag;
    ULONGLONG frame = 0;
};
Demo g_demoState[3];
HWND g_demo[3];
HWND g_scratch = nullptr;

LRESULT CALLBACK DemoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == 1) {
        auto* d = reinterpret_cast<Demo*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        d->frame++;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        auto* d = reinterpret_cast<Demo*>(GetWindowLongPtrW(h, GWLP_USERDATA));

        HBRUSH bg = CreateSolidBrush(RGB(14, 18, 30));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);

        // A strip that slides across so liveness is obvious in stills.
        int x = static_cast<int>(d->frame % 8) * (rc.right / 8);
        RECT strip{x + 20, rc.top + 12, x + 90, rc.top + 42};
        HBRUSH br = CreateSolidBrush(RGB(230, 120, 60));
        FillRect(dc, &strip, br);
        DeleteObject(br);

        wchar_t text[64];
        swprintf(text, 64, L"%c frame %llu", d->tag, static_cast<unsigned long long>(d->frame));
        HFONT f = CreateFontW(120, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, f);
        SetTextColor(dc, RGB(190, 230, 255));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);
        DeleteObject(f);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, m, w, l);
}

// --------------------------- PNG capture (WIC) ------------------------------

void SavePng(HDC hdc, HBITMAP bmp, int w, int h, std::wstring const& path) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> bits(static_cast<size_t>(w) * h * 4);
    if (!GetDIBits(hdc, bmp, 0, h, bits.data(), &bi, DIB_RGB_COLORS)) {
        Logf(L"GetDIBits failed %lu\n", GetLastError());
        return;
    }
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        Logf(L"WIC factory failed\n");
        return;
    }
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    HRESULT hr = f->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&fr, nullptr);
    if (SUCCEEDED(hr)) hr = fr->Initialize(nullptr);
    if (SUCCEEDED(hr)) hr = fr->SetSize(w, h);
    if (SUCCEEDED(hr)) hr = fr->WritePixels(h, w * 4, w * 4 * h, bits.data());
    if (fr) fr->Commit();
    if (enc) enc->Commit();
    if (stream) stream->Commit(STGC_DEFAULT);
    if (FAILED(hr)) Logf(L"PNG write failed 0x%08X %s\n", hr, path.c_str());
    else Logf(L" wrote %s\n", path.c_str());
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    f->Release();
}

// Grabs the window's rect from the screen, or the window content via PrintWindow.
void Shot(HWND wnd, std::wstring const& base, std::wstring const& dir, bool print) {
    RECT rc;
    GetWindowRect(wnd, &rc);
    int x = rc.left, y = rc.top, w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    if (print) {
        PrintWindow(wnd, mem, PW_RENDERFULLCONTENT);
        SavePng(mem, bmp, w, h, dir + L"\\" + base + L"-full.png");
        Sleep(30);
        PrintWindow(wnd, mem, 0);
        SavePng(mem, bmp, w, h, dir + L"\\" + base + L"-plain.png");
    } else {
        BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY);
        SavePng(mem, bmp, w, h, dir + L"\\" + base + L"-screen.png");
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ----------------------------- thumbnails ----------------------------------

struct Registered {
    HWND src{};
    HTHUMBNAIL th{};
};
std::vector<Registered> g_thumbs;

bool RegisterOne(HWND dest, HWND src, RECT const& destClient) {
    HTHUMBNAIL id = nullptr;
    HRESULT hr = DwmRegisterThumbnail(dest, src, &id);
    Logf(L"  register src=%p -> hr=0x%08X %hs id=%p\n", (void*)src, hr, HrName(hr), (void*)id);
    if (FAILED(hr)) return false;
    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
    p.rcDestination = destClient;
    p.fSourceClientAreaOnly = TRUE;
    p.fVisible = TRUE;
    p.opacity = 255;
    HRESULT hr2 = DwmUpdateThumbnailProperties(id, &p);
    Logf(L"  props -> hr=0x%08X %hs\n", hr2, HrName(hr2));
    if (FAILED(hr2)) {
        DwmUnregisterThumbnail(id);
        return false;
    }
    g_thumbs.push_back({src, id});
    return true;
}

void UnregisterAll() {
    for (auto& t : g_thumbs) {
        HRESULT hr = DwmUnregisterThumbnail(t.th);
        if (FAILED(hr)) Logf(L"unregister failed 0x%08X\n", hr);
    }
    g_thumbs.clear();
}

// One rounded-rect region covering the card rects, for the region test.
HRGN CardsRegion(std::vector<RECT> const& cards, int radius) {
    HRGN acc = nullptr;
    for (auto const& r : cards) {
        HRGN one = CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, radius, radius);
        if (acc) {
            CombineRgn(acc, acc, one, RGN_OR);
            DeleteObject(one);
        } else {
            acc = one;
        }
    }
    return acc;
}

// Card layout: fixed height, width from the source's client aspect.
struct CardInfo {
    RECT thumbRect;  // client coords of the destination window
    HWND src;
};
std::vector<CardInfo> LayCards(int inset, int gap, int cardH, std::vector<std::pair<HWND, RECT>> const& srcs) {
    std::vector<CardInfo> out;
    int x = inset;
    for (auto const& s : srcs) {
        int sw = s.second.right - s.second.left, sh = s.second.bottom - s.second.top;
        if (sw <= 0 || sh <= 0) { sw = 400; sh = 300; }
        int cw = cardH * sw / sh;
        out.push_back(CardInfo{{x, inset, x + cw, inset + cardH}, s.first});
        x += cw + gap;
    }
    return out;
}

// ------------------------------- main --------------------------------------

LRESULT CALLBACK NothingProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring dir = argc > 1 ? argv[1] : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring logPath = dir + L"\\thumbtest.log";
    g_log = _wfopen(logPath.c_str(), L"wt");

    Logf(L"[thumbtest] out dir %s; pid %lu\n", dir.c_str(), GetCurrentProcessId());

    // Composition for variant D.
    DispatcherQueueOptions dqo{sizeof(dqo), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    ABI::Windows::System::IDispatcherQueueController* dqRaw = nullptr;
    HRESULT dqr = CreateDispatcherQueueController(dqo, &dqRaw);
    Logf(L"dispatcher queue hr=0x%08X\n", dqr);
    winrt::com_ptr<ABI::Windows::System::IDispatcherQueueController> dq;
    if (dqRaw) { dq.copy_from(dqRaw); dqRaw->Release(); }
    wuc::Compositor comp;

    // Demo content: two visible, one minimized.
    WNDCLASSW dcw{};
    dcw.lpfnWndProc = DemoProc;
    dcw.hInstance = inst;
    dcw.lpszClassName = L"Ht.Demo";
    RegisterClassW(&dcw);
    DWORD const style = WS_POPUP | WS_VISIBLE;
    g_demo[0] = CreateWindowExW(0, L"Ht.Demo", L"demo A", style, 40, 700, 500, 375, nullptr, nullptr, inst, nullptr);
    g_demo[1] = CreateWindowExW(0, L"Ht.Demo", L"demo B", style, 40, 1100, 500, 375, nullptr, nullptr, inst, nullptr);
    g_demo[2] = CreateWindowExW(0, L"Ht.Demo", L"demo C (min)", style, 560, 1100, 500, 375, nullptr, nullptr, inst, nullptr);
    for (int i = 0; i < 3; i++) {
        g_demoState[i].tag = L'A' + static_cast<wchar_t>(i);
        SetWindowLongPtrW(g_demo[i], GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&g_demoState[i]));
        SetTimer(g_demo[i], 1, 400, nullptr);
        InvalidateRect(g_demo[i], nullptr, FALSE);
    }
    RECT clientA{}, clientB{};
    GetClientRect(g_demo[0], &clientA);
    GetClientRect(g_demo[1], &clientB);
    ShowWindow(g_demo[0], SW_SHOWNOACTIVATE);
    ShowWindow(g_demo[1], SW_SHOWNOACTIVATE);
    Logf(L"demo A visible=%d iconic=%d client %ldx%ld\n", IsWindowVisible(g_demo[0]), IsIconic(g_demo[0]),
         clientA.right, clientA.bottom);
    ShowWindow(g_demo[2], SW_MINIMIZE);
    Logf(L"demo C iconic=%d\n", IsIconic(g_demo[2]));

    // Destination classes (all: no activation, topmost, tool — like the dock).
    WNDCLASSW nw{};
    nw.lpfnWndProc = NothingProc;
    nw.hInstance = inst;
    nw.lpszClassName = L"Ht.Plain";
    RegisterClassW(&nw);

    DWORD const exTop = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
    std::vector<HWND> dests;

    auto MakeDest = [&](DWORD ex, int x, int y, int w, int h) {
        HWND h2 = CreateWindowExW(ex, L"Ht.Plain", L"", WS_POPUP, x, y, w, h, nullptr, nullptr, inst, nullptr);
        dests.push_back(h2);
        return h2;
    };

    MARGINS mgn{-1, -1, -1, -1};

    // A: plain + glass frame extension. Cards: [A, B, minimized C].
    HWND va = MakeDest(exTop, 80, 120, 870, 232);
    DwmExtendFrameIntoClientArea(va, &mgn);
    {
        auto cards = LayCards(16, 10, 200, {{g_demo[0], clientA}, {g_demo[1], clientB}, {g_demo[2], clientA}});
        Logf(L"variant A cards=%zu\n", cards.size());
        for (auto const& c : cards) RegisterOne(va, c.src, c.thumbRect);
        ShowWindow(va, SW_SHOWNOACTIVATE);
        Logf(L"variant A: thumbs registered BEFORE show (visible=%d)\n", IsWindowVisible(va));
    }

    // C: layered per-pixel background, then register thumbs on it.
    HWND vc = MakeDest(exTop | WS_EX_LAYERED, 980, 120, 592, 232);
    {
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, 592, 232);
        HGDIOBJ old = SelectObject(mem, bmp);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = 592;
        bi.bmiHeader.biHeight = -232;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint32_t> bg(size_t(592) * 232, 0xC03A2032u);  // premultiplied ARGB bg
        SetDIBitsToDevice(mem, 0, 0, 592, 232, 0, 0, 0, 232, bg.data(), &bi, DIB_RGB_COLORS);
        POINT srcpt{0, 0};
        SIZE size{592, 232};
        POINT dst{980, 120};
        BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        BOOL okULW = UpdateLayeredWindow(vc, screen, &dst, &size, mem, &srcpt, 0, &bf, ULW_ALPHA);
        Logf(L"variant C ULW ok=%d err=%lu\n", okULW, GetLastError());
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        auto cards = LayCards(16, 10, 200, {{g_demo[0], clientA}, {g_demo[1], clientB}});
        for (auto const& c : cards) RegisterOne(vc, c.src, c.thumbRect);
        ShowWindow(vc, SW_SHOWNOACTIVATE);
    }

    // D: the dock's own stack: NOREDIRECTIONBITMAP + Composition visuals.
    HWND vd = MakeDest(WS_EX_NOREDIRECTIONBITMAP | exTop, 80, 420, 592, 232);
    {
        auto interop = comp.as<ICompositorDesktopInterop>();
        winrt::Windows::UI::Composition::Desktop::DesktopWindowTarget target{nullptr};
        HRESULT hr = interop->CreateDesktopWindowTarget(vd, TRUE, winrt::put_abi(target));
        Logf(L"variant D desktop target hr=0x%08X\n", hr);
        if (SUCCEEDED(hr)) {
            auto root = comp.CreateContainerVisual();
            root.RelativeSizeAdjustment({1, 1});
            target.Root(root);
            // Two flat cards behind where the thumbnails will land.
            int x = 16;
            for (int i = 0; i < 2; i++) {
                auto card = comp.CreateSpriteVisual();
                card.Size({266.7f, 200});
                card.Offset({static_cast<float>(x), 16, 0});
                card.Brush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0xA8, 0x1E, 0x22, 0x2E)));
                root.Children().InsertAtTop(card);
                x += 277;
            }
        }
        auto cards = LayCards(16, 10, 200, {{g_demo[0], clientA}, {g_demo[1], clientB}});
        for (auto const& c : cards) RegisterOne(vd, c.src, c.thumbRect);
        ShowWindow(vd, SW_SHOWNOACTIVATE);
    }

    // F: plain with a rounded window region over the card rects.
    HWND vf = MakeDest(exTop, 980, 420, 592, 232);
    DwmExtendFrameIntoClientArea(vf, &mgn);
    {
        auto cards = LayCards(16, 10, 200, {{g_demo[0], clientA}, {g_demo[1], clientB}});
        std::vector<RECT> rects;
        for (auto const& c : cards) rects.push_back(c.thumbRect);
        HRGN rg = CardsRegion(rects, 22);
        BOOL okr = SetWindowRgn(vf, rg, TRUE);
        Logf(L"variant F region ok=%d\n", okr);
        for (auto const& c : cards) RegisterOne(vf, c.src, c.thumbRect);
        ShowWindow(vf, SW_SHOWNOACTIVATE);
    }

    // Scratch: never shown. Does registration succeed while hidden?
    g_scratch = CreateWindowExW(exTop, L"Ht.Plain", L"", WS_POPUP, -4000, -4000, 300, 230, nullptr, nullptr, inst, nullptr);
    {
        Logf(L"hidden destination test:\n");
        BOOL vis = IsWindowVisible(g_scratch);
        Logf(L"  (hidden=%d)\n", !vis);
        HTHUMBNAIL id = nullptr;
        HRESULT hr = DwmRegisterThumbnail(g_scratch, g_demo[0], &id);
        Logf(L"  register on hidden dest hr=0x%08X %hs\n", hr, HrName(hr));
        if (SUCCEEDED(hr)) DwmUnregisterThumbnail(id);
    }
    // A source that no longer exists.
    {
        HWND dead = CreateWindowExW(0, L"Ht.Demo", L"dead", 0, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
        DestroyWindow(dead);
        Logf(L"destroyed source test on variant A:\n");
        RECT r{750, 16, 850, 116};
        RegisterOne(va, dead, r);
    }

    // Let DWM composite, then shoot.
    Sleep(800);
    Logf(L"--- shots 1 ---\n");
    for (size_t i = 0; i < dests.size(); i++) {
        wchar_t n[8];
        swprintf(n, 8, L"v%zu", i);
        Shot(dests[i], std::wstring(n) + L"-screen1", dir, false);
        Shot(dests[i], std::wstring(n) + L"-print1", dir, true);
    }
    Sleep(1400);
    Logf(L"--- shots 2 (frames should differ) ---\n");
    for (size_t i = 0; i < dests.size(); i++) {
        wchar_t n[8];
        swprintf(n, 8, L"v%zu", i);
        Shot(dests[i], std::wstring(n) + L"-screen2", dir, false);
        Shot(dests[i], std::wstring(n) + L"-print2", dir, true);
    }

    // Handle counting across register/unregister cycles.
    auto handles = [] {
        DWORD c = 0;
        GetProcessHandleCount(GetCurrentProcess(), &c);
        return c;
    };
    auto gdi = [] { return GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS); };
    auto userObjs = [] { return GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS); };
    DWORD h0 = handles(), g0 = gdi(), u0 = userObjs();
    for (int i = 0; i < 300; i++) {
        RECT r{16, 16, 216, 166};
        (void)r;
        HTHUMBNAIL id = nullptr;
        if (SUCCEEDED(DwmRegisterThumbnail(va, g_demo[i % 2], &id))) DwmUnregisterThumbnail(id);
    }
    for (int i = 0; i < 300; i++) {
        HTHUMBNAIL id = nullptr;
        if (SUCCEEDED(DwmRegisterThumbnail(g_scratch, g_demo[i % 2], &id))) DwmUnregisterThumbnail(id);
    }
    DWORD h1 = handles(), g1 = gdi(), u1 = userObjs();
    Logf(L"300 visible + 300 hidden register/unregister cycles:\n");
    Logf(L"  kernel handles %lu -> %lu (+%ld)\n", h0, h1, static_cast<long>(h1) - static_cast<long>(h0));
    Logf(L"  GDI objects %lu -> %lu (+%ld)\n", g0, g1, static_cast<long>(g1) - static_cast<long>(g0));
    Logf(L"  USER objects %lu -> %lu (+%ld)\n", u0, u1, static_cast<long>(u1) - static_cast<long>(u0));

    // Clean up: unregister everything, then destroy the windows.
    UnregisterAll();
    for (HWND d : dests) DestroyWindow(d);
    for (HWND d : g_demo) DestroyWindow(d);
    DestroyWindow(g_scratch);
    Logf(L"done; windows destroyed\n");
    if (g_log) fclose(g_log);
    CoUninitialize();
    return 0;
}
