// A look at the Start panel (Launcher), hosted in isolation, saved as a PNG of ONLY its own windows. Like start_perf it makes its
// own compositor, graphics device and a copy of the dock's glass, builds the real halo::Launcher (src/launcher.cpp, linked in)
// and never starts halo.exe, installs a hook or touches the live shell.
//
// Everything the panel shows is stand-in content: sample app names (lettered icons), sample files (shortcuts to empty files in a
// temp folder, so no real Recent entry is read), sample notifications (HALO_NOTIF_DEMO), a stand-in user name, no services (the
// control column shows made-up radio/volume state). Settings are a scratch file. The desktop behind the glass is a stand-in
// window of this process (a gradient wallpaper and a fake document), so what the glass blurs is known and nothing real shows
// through it.
//
// The capture is a Desktop Duplication frame of which only the rectangle of the stand-in backdrop around the panel is copied (a
// region copy into a staging texture of that size): the rest of the desktop is never read into memory, let alone saved.
//
//   start_preview.exe --out file.png [--mode h2|classic] [--theme dark|light] [--query TEXT] [--notifs demo|3|0] [--menu]
//                     [--hover app0|file0|power|settings|lock] [--press] [--hold-ms 2200]
// The window appears for about three seconds. Build: tools/build.sh start_preview
#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dispatcherqueue.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <windows.ui.composition.interop.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "../src/launcher.h"
#include "../src/settings.h"
#include "../src/theme.h"

extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

#ifndef HALO_COMPOSITION_INTEROP_DEFINED
#define HALO_COMPOSITION_INTEROP_DEFINED
struct ICompositionDrawingSurfaceInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginDraw(RECT const* updateRect, REFIID iid, void** updateObject, POINT* offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resize(SIZE sizePixels) = 0;
    virtual HRESULT STDMETHODCALLTYPE Scroll(RECT const* scrollRect, RECT const* clipRect, int offsetX, int offsetY) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendDraw() = 0;
};
__CRT_UUID_DECL(ICompositionDrawingSurfaceInterop, 0xFD04E6E3, 0xFE0C, 0x4C3C, 0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE)
#endif

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Size;

namespace {

LONGLONG g_qf = 0;
LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
LONGLONG After(LONGLONG t, double ms) { return t + static_cast<LONGLONG>(ms * g_qf / 1000.0); }

void Pump(LONGLONG until) {
    while (true) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        LONGLONG left = until - Now();
        if (left <= 0) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>(std::min<LONGLONG>(left * 1000 / g_qf, 4)), QS_ALLINPUT);
    }
}

struct Env {
    winrt::Windows::System::DispatcherQueueController dq{nullptr};
    wuc::Compositor comp{nullptr};
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<ID2D1Device> d2d;
    wuc::CompositionGraphicsDevice gdev{nullptr};
    float scale = 1.0f;
    RECT monitor{};
};

wuc::CompositionDrawingSurface MakeSurface(Env& e, Size size) {
    return e.gdev.CreateDrawingSurface(size, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                       winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
}

// The grain: xorshift noise at w x h, as halo.cpp draws it.
wuc::CompositionBrush NoiseBrush(Env& e, int w, int h) {
    auto surface = MakeSurface(e, {static_cast<float>(w), static_cast<float>(h)});
    auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset));
    std::vector<uint32_t> px32(static_cast<size_t>(w) * h);
    uint32_t seed = 0x2545F491;
    for (auto& p : px32) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        uint32_t g = seed & 0xFF;
        p = 0xFF000000 | g << 16 | g << 8 | g;
    }
    winrt::com_ptr<ID2D1Bitmap> bitmap;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), px32.data(), w * 4, &props, bitmap.put()));
    D2D1_RECT_F dest = D2D1::RectF(float(offset.x), float(offset.y), float(offset.x + w), float(offset.y + h));
    dc->DrawBitmap(bitmap.get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
    winrt::check_hresult(interop->EndDraw());
    auto brush = e.comp.CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::None);
    return brush;
}

// The dock's glass, as Halo::BuildPill builds it (halo.cpp): blurred host backdrop, tint, grain, gradient rim, outer hairline.
wuc::ContainerVisual BuildGlass(Env& e, int noiseHeight, halo::settings::Surface const& cfg, int noiseWidth, float radius) {
    auto& comp = e.comp;
    auto pill = comp.CreateContainerVisual();
    auto glass = comp.CreateSpriteVisual();
    glass.RelativeSizeAdjustment({1, 1});
    glass.Brush(comp.CreateHostBackdropBrush());
    pill.Children().InsertAtTop(glass);
    auto tint = comp.CreateSpriteVisual();
    tint.RelativeSizeAdjustment({1, 1});
    tint.Brush(comp.CreateColorBrush(halo::theme::C(halo::theme::Glass(cfg).tint)));
    pill.Children().InsertAtTop(tint);
    auto noise = comp.CreateSpriteVisual();
    noise.RelativeSizeAdjustment({1, 1});
    noise.Opacity(halo::theme::P().noise);
    noise.Brush(NoiseBrush(e, noiseWidth ? noiseWidth : e.monitor.right - e.monitor.left, noiseHeight ? noiseHeight : 80));
    pill.Children().InsertAtTop(noise);

    const float t = std::max(1.0f, std::round(e.scale));
    const float r = (radius > 0 ? radius : 20 * e.scale) - t / 2;
    auto rimShape = comp.CreateRoundedRectangleGeometry();
    auto const& pal = halo::theme::P();
    const bool hasEdge = pal.edge.a != 0;
    const float ri = hasEdge ? t : 0.0f;
    rimShape.Offset({t / 2 + ri, t / 2 + ri});
    rimShape.CornerRadius({r - ri, r - ri});
    auto rimSize = comp.CreateExpressionAnimation(L"Vector2(pill.Size.X - " + std::to_wstring(t + 2 * ri) + L", pill.Size.Y - " + std::to_wstring(t + 2 * ri) + L")");
    rimSize.SetReferenceParameter(L"pill", pill);
    rimShape.StartAnimation(L"Size", rimSize);
    auto gradient = comp.CreateLinearGradientBrush();
    gradient.MappingMode(wuc::CompositionMappingMode::Relative);
    gradient.StartPoint({0, 0});
    gradient.EndPoint({0, 1});
    auto stop = [&](float at, halo::theme::Argb c) { gradient.ColorStops().Append(comp.CreateColorGradientStop(at, halo::theme::C(c))); };
    stop(0.0f, pal.rimTop); stop(0.35f, pal.rimMid); stop(0.7f, pal.rimLow); stop(1.0f, pal.rimBottom);
    auto rimSprite = comp.CreateSpriteShape(rimShape);
    rimSprite.StrokeBrush(gradient);
    rimSprite.StrokeThickness(t);
    auto rim = comp.CreateShapeVisual();
    rim.RelativeSizeAdjustment({1, 1});
    rim.Shapes().Append(rimSprite);
    pill.Children().InsertAtTop(rim);
    if (hasEdge) {
        auto edgeShape = comp.CreateRoundedRectangleGeometry();
        edgeShape.Offset({t / 2, t / 2});
        edgeShape.CornerRadius({r, r});
        auto edgeSize = comp.CreateExpressionAnimation(L"Vector2(pill.Size.X - " + std::to_wstring(t) + L", pill.Size.Y - " + std::to_wstring(t) + L")");
        edgeSize.SetReferenceParameter(L"pill", pill);
        edgeShape.StartAnimation(L"Size", edgeSize);
        auto edgeSprite = comp.CreateSpriteShape(edgeShape);
        edgeSprite.StrokeBrush(comp.CreateColorBrush(halo::theme::C(pal.edge)));
        edgeSprite.StrokeThickness(t);
        auto edge = comp.CreateShapeVisual();
        edge.RelativeSizeAdjustment({1, 1});
        edge.Shapes().Append(edgeSprite);
        pill.Children().InsertAtTop(edge);
    }
    return pill;
}

// A lettered rounded-square stand-in for an app's icon (the native build keeps the real icons; the mock uses lettered tiles too).
wuc::CompositionBrush LetterIcon(Env& e, winrt::com_ptr<IDWriteFactory> const& dw, std::wstring const& name, float px) {
    static const std::map<std::wstring, uint32_t> colours = {{L"Edge", 0x1f8fd6}, {L"Terminal", 0x2d2d33}, {L"Code", 0x2a7fcf}, {L"Explorer", 0xe3a322},
        {L"Spotify", 0x1fa855}, {L"Notes", 0xd0563f}, {L"Figma", 0x8a5cd6}, {L"Kompass", 0x27798a}, {L"Mail", 0x2b7cd3}, {L"Photos", 0xc2478f},
        {L"Calc", 0x4d5560}, {L"Store", 0x3b6fd1}};
    uint32_t rgb = 0x5b6b7b;
    if (auto it = colours.find(name); it != colours.end()) rgb = it->second;
    else for (wchar_t c : name) rgb = ((rgb * 31 + c) & 0xFFFFFF) | 0x303030;
    auto surface = MakeSurface(e, {px, px});
    auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT off{};
    winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off));
    dc->Clear(D2D1::ColorF(0, 0.0f));
    winrt::com_ptr<ID2D1SolidColorBrush> fill, white;
    dc->CreateSolidColorBrush(D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, 1.0f), fill.put());
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), white.put());
    const D2D1_RECT_F r = D2D1::RectF(off.x + px * 0.04f, off.y + px * 0.04f, off.x + px * 0.96f, off.y + px * 0.96f);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, px * 0.22f, px * 0.22f), fill.get());
    winrt::com_ptr<IDWriteTextFormat> fmt;
    dw->CreateTextFormat(L"Segoe UI Variable Display", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px * 0.4f, L"en-us", fmt.put());
    if (fmt) {
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        std::wstring letter(1, name.empty() ? L'?' : name[0]);
        dc->DrawText(letter.c_str(), 1, fmt.get(), r, white.get());
    }
    winrt::check_hresult(interop->EndDraw());
    auto brush = e.comp.CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Uniform);
    return brush;
}

// ---- the stand-in desktop behind the glass: a gradient wallpaper with a fake document window ----
bool g_dark = true;
LRESULT CALLBACK BackdropProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        const int W = rc.right, H = rc.bottom;
        const COLORREF top = g_dark ? RGB(0x0c, 0x12, 0x20) : RGB(0xee, 0xf2, 0xf6), mid = g_dark ? RGB(0x17, 0x40, 0x4a) : RGB(0xf6, 0xd9, 0xb8);
        for (int y = 0; y < H; y++) {  // vertical blend, wallpaper-ish
            const float t = static_cast<float>(y) / std::max(1, H - 1);
            auto lerp = [&](int a, int b) { return static_cast<int>(a + (b - a) * t); };
            HBRUSH b = CreateSolidBrush(RGB(lerp(GetRValue(top), GetRValue(mid)), lerp(GetGValue(top), GetGValue(mid)), lerp(GetBValue(top), GetBValue(mid))));
            RECT line{0, y, W, y + 1};
            FillRect(dc, &line, b);
            DeleteObject(b);
        }
        // a stand-in app window, so the glass is judged over content (bright, with text), like the mock's #win
        RECT win{W / 14, H / 12, W * 11 / 14, H * 9 / 12};
        HBRUSH wb = CreateSolidBrush(RGB(0xfb, 0xfb, 0xfb));
        FillRect(dc, &win, wb);
        DeleteObject(wb);
        RECT bar{win.left, win.top, win.right, win.top + 36};
        HBRUSH bb = CreateSolidBrush(RGB(0xec, 0xec, 0xec));
        FillRect(dc, &bar, bb);
        DeleteObject(bb);
        SetBkMode(dc, TRANSPARENT);
        HFONT f = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT old = static_cast<HFONT>(SelectObject(dc, f));
        SetTextColor(dc, RGB(0x22, 0x22, 0x22));
        TextOutW(dc, win.left + 28, win.top + 60, L"A window behind the glass", 25);
        HFONT f2 = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, f2);
        SetTextColor(dc, RGB(0x44, 0x44, 0x44));
        for (int i = 0; i < 18; i++) {
            const wchar_t* line = L"This stands in for whatever app is open: text, edges and a bright background show through the blur.";
            TextOutW(dc, win.left + 28, win.top + 110 + i * 22, line, static_cast<int>(wcslen(line)));
        }
        SelectObject(dc, old);
        DeleteObject(f);
        DeleteObject(f2);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

HWND MakeBackdrop(RECT r) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = BackdropProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"HaloStartPreviewBackdrop";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, wc.lpszClassName, L"", WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    SetWindowPos(h, HWND_TOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(h);
    return h;
}

HWND FindPanel() {
    HWND found = nullptr;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM l) -> BOOL {
        wchar_t cls[64];
        GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"HaloLauncherWindow")) { *reinterpret_cast<HWND*>(l) = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    return found;
}

// ---- capture: one Desktop Duplication frame, of which only `crop` is copied out and saved as a PNG ----
bool CaptureCrop(RECT crop, std::wstring const& pngPath) {
    winrt::com_ptr<ID3D11Device> dev;
    winrt::com_ptr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put()))) return false;
    auto xd = dev.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> ad;
    xd->GetAdapter(ad.put());
    winrt::com_ptr<IDXGIOutput> out;
    if (FAILED(ad->EnumOutputs(0, out.put()))) return false;
    DXGI_OUTPUT_DESC od{};
    out->GetDesc(&od);
    winrt::com_ptr<IDXGIOutputDuplication> dup;
    if (FAILED(out.as<IDXGIOutput1>()->DuplicateOutput(dev.get(), dup.put()))) return false;

    const UINT w = crop.right - crop.left, h = crop.bottom - crop.top;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, staging.put()))) return false;

    bool got = false;
    for (int i = 0; i < 12; i++) {
        DXGI_OUTDUPL_FRAME_INFO fi{};
        winrt::com_ptr<IDXGIResource> res;
        HRESULT hr = dup->AcquireNextFrame(got ? 250 : 1000, &fi, res.put());
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) { if (got) break; continue; }
        if (FAILED(hr)) break;
        if (fi.LastPresentTime.QuadPart != 0 || !got) {
            auto tex = res.as<ID3D11Texture2D>();
            D3D11_BOX box{static_cast<UINT>(crop.left - od.DesktopCoordinates.left), static_cast<UINT>(crop.top - od.DesktopCoordinates.top), 0,
                          static_cast<UINT>(crop.right - od.DesktopCoordinates.left), static_cast<UINT>(crop.bottom - od.DesktopCoordinates.top), 1};
            ctx->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, tex.get(), 0, &box);  // only the crop leaves the GPU
            got = true;
        }
        res = nullptr;
        dup->ReleaseFrame();
    }
    if (!got) return false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    bool ok = false;
    {
        winrt::com_ptr<IWICImagingFactory> wic;
        winrt::com_ptr<IWICStream> stream;
        winrt::com_ptr<IWICBitmapEncoder> enc;
        winrt::com_ptr<IWICBitmapFrameEncode> frame;
        winrt::com_ptr<IPropertyBag2> bag;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), wic.put_void())) &&
            SUCCEEDED(wic->CreateStream(stream.put())) && SUCCEEDED(stream->InitializeFromFilename(pngPath.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) && SUCCEEDED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) &&
            SUCCEEDED(enc->CreateNewFrame(frame.put(), bag.put())) && SUCCEEDED(frame->Initialize(bag.get())) && SUCCEEDED(frame->SetSize(w, h))) {
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
            frame->SetPixelFormat(&fmt);
            std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);  // tightly packed, alpha forced opaque (a desktop frame's alpha means nothing)
            for (UINT y = 0; y < h; y++) {
                memcpy(&px[static_cast<size_t>(y) * w * 4], static_cast<BYTE*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch, static_cast<size_t>(w) * 4);
                for (UINT x = 0; x < w; x++) px[(static_cast<size_t>(y) * w + x) * 4 + 3] = 0xFF;
            }
            ok = fmt == GUID_WICPixelFormat32bppBGRA && SUCCEEDED(frame->WritePixels(h, w * 4, static_cast<UINT>(px.size()), px.data())) && SUCCEEDED(frame->Commit()) &&
                 SUCCEEDED(enc->Commit());
        }
    }
    ctx->Unmap(staging.get(), 0);
    return ok;
}

void WriteText(std::wstring const& path, std::string const& s) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(s.data(), 1, s.size(), f);
    fclose(f);
}

std::string Narrow(std::wstring const& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// A shortcut named `name`.lnk in `dir`, to an (empty) file in `docs`, last written `hoursAgo` hours ago.
void MakeRecent(std::wstring const& dir, std::wstring const& docs, std::wstring const& name, double hoursAgo) {
    const std::wstring target = docs + L"\\" + name;
    HANDLE f = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    winrt::com_ptr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, link.put_void()))) return;
    link->SetPath(target.c_str());
    const std::wstring lnk = dir + L"\\" + name + L".lnk";
    if (FAILED(link.as<IPersistFile>()->Save(lnk.c_str(), TRUE))) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULONGLONG t = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - static_cast<ULONGLONG>(hoursAgo * 3600.0 * 1e7);
    ft = {static_cast<DWORD>(t & 0xFFFFFFFF), static_cast<DWORD>(t >> 32)};
    HANDLE h = CreateFileW(lnk.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { SetFileTime(h, nullptr, nullptr, &ft); CloseHandle(h); }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    QueryPerformanceFrequency(reinterpret_cast<LARGE_INTEGER*>(&g_qf));
    std::wstring out, mode = L"h2", theme = L"dark", query, notifs = L"3", hover;
    bool menu = false, press = false;
    int holdMs = 2200;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--out") out = next();
        else if (a == L"--mode") mode = next();
        else if (a == L"--theme") theme = next();
        else if (a == L"--query") query = next();
        else if (a == L"--notifs") notifs = next();
        else if (a == L"--hover") hover = next();
        else if (a == L"--hold-ms") holdMs = _wtoi(next().c_str());
        else if (a == L"--menu") menu = true;
        else if (a == L"--press") press = true;
    }
    if (out.empty()) { std::fprintf(stderr, "usage: start_preview --out file.png [--mode h2|classic] [--theme dark|light] [--query TEXT] [--notifs demo|3|0] [--menu] [--hover app0|file0|power|settings|lock] [--press]\n"); return 2; }
    const bool h2 = mode == L"h2";
    g_dark = theme != L"light";

    // ---- stand-in content and scratch state ----
    wchar_t tmp[MAX_PATH];
    GetEnvironmentVariableW(L"TEMP", tmp, MAX_PATH);
    const std::wstring dir = std::wstring(tmp) + L"\\halo-start-preview-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring docs = dir + L"\\docs", recentDir = dir + L"\\recent";
    CreateDirectoryW(dir.c_str(), nullptr);
    CreateDirectoryW(docs.c_str(), nullptr);
    CreateDirectoryW(recentDir.c_str(), nullptr);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const std::vector<std::wstring> apps = {L"Edge", L"Terminal", L"Code", L"Explorer", L"Spotify", L"Notes", L"Figma", L"Kompass", L"Mail", L"Photos", L"Calc", L"Store"};
    const std::vector<std::wstring> moreApps = {L"Calendar", L"Camera", L"Clock", L"Contacts", L"Copilot", L"Settings Demo"};
    std::string mru, index;
    for (auto const& a : apps) {
        mru += Narrow(L"C:\\Sample\\" + a + L".lnk") + "\n";
        index += Narrow(a + L"|C:\\Sample\\" + a + L".lnk") + "\n";
    }
    for (auto const& a : moreApps) index += Narrow(a + L"|C:\\Sample\\" + a + L".lnk") + "\n";
    WriteText(dir + L"\\recent.txt", mru);
    WriteText(dir + L"\\apps.txt", index);
    struct F { const wchar_t* name; double hours; } files[] = {{L"lab-07-graph.cpp", 0.5}, {L"halo-directions.html", 3}, {L"plan-2026-11.md", 26}, {L"invoice-october.pdf", 50},
        {L"screenshot-start.png", 75}, {L"thesis-notes.docx", 140}, {L"budget.xlsx", 200}, {L"slides-final.pptx", 260}, {L"readme.md", 400}, {L"icons.zip", 480}};
    for (auto const& f : files) MakeRecent(recentDir, docs, f.name, f.hours);
    {
        std::string ini = std::string("[general]\ntheme = ") + (g_dark ? "dark" : "light") + "\nreduce_motion = off\nhalo2 = " + (h2 ? "on" : "off") +
                          "\n[start]\neffect = none\nrecent_apps = 6\nrecent_files = 12\ndim_desktop = off\n";
        WriteText(dir + L"\\settings.ini", ini);
    }
    SetEnvironmentVariableW(L"HALO_SETTINGS_FILE", (dir + L"\\settings.ini").c_str());
    SetEnvironmentVariableW(L"HALO_TEST_RECENT", (dir + L"\\recent.txt").c_str());
    SetEnvironmentVariableW(L"HALO_TEST_APPS", (dir + L"\\apps.txt").c_str());
    SetEnvironmentVariableW(L"HALO_TEST_RECENT_DIR", recentDir.c_str());
    SetEnvironmentVariableW(L"HALO_TEST_USER", L"Alex Sample");
    SetEnvironmentVariableW(L"HALO_THEME", g_dark ? L"dark" : L"light");
    if (notifs == L"0") SetEnvironmentVariableW(L"HALO_NOTIF_EMPTY", L"1");
    else SetEnvironmentVariableW(L"HALO_NOTIF_DEMO", notifs == L"demo" ? L"1" : L"3");
    // notif_feed.cpp reads its switches with the CRT's _wgetenv, which does not see SetEnvironmentVariableW: set those through the CRT too
    // (a missed HALO_NOTIF_DEMO would draw the machine's real notifications).
    _wputenv_s(L"HALO_TEST", L"1");
    _wputenv_s(L"HALO_NOTIF_DRYRUN", L"1");
    if (notifs == L"0") _wputenv_s(L"HALO_NOTIF_EMPTY", L"1");
    else _wputenv_s(L"HALO_NOTIF_DEMO", notifs == L"demo" ? L"1" : L"3");
    for (auto n : {L"HALO_TEST", L"HALO_TEST_STAY", L"HALO_TEST_NOFOCUS", L"HALO_TEST_POWER_DRYRUN", L"HALO_TEST_CC_DRYRUN", L"HALO_TEST_NOSERVICES"}) SetEnvironmentVariableW(n, L"1");
    FILE* lg = nullptr;
    _wfreopen_s(&lg, (dir + L"\\launcher.log").c_str(), L"w", stderr);  // test points and timings

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    halo::settings::Reload();
    halo::theme::Refresh();
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Env e;
    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::check_hresult(CreateDispatcherQueueController(opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(e.dq))));
    e.comp = wuc::Compositor();
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, e.d3d.put(), nullptr, nullptr));
    winrt::check_hresult(D2D1CreateDevice(e.d3d.as<IDXGIDevice>().get(), nullptr, e.d2d.put()));
    auto interop = e.comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    winrt::check_hresult(interop->CreateGraphicsDevice(reinterpret_cast<IUnknown*>(e.d2d.get()),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(e.gdev))));
    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    e.monitor = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    e.scale = dx / 96.0f;

    winrt::com_ptr<IDWriteFactory> dw;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.put()));
    halo::LauncherHost host;
    host.glass = [&](int nh, halo::settings::Surface const& g, int nw, float radius) { return BuildGlass(e, nh, g, nw, radius); };
    std::map<std::wstring, wuc::CompositionBrush> brushes;
    host.appIcon = [&](std::wstring const& target) -> wuc::CompositionBrush {
        auto it = brushes.find(target);
        if (it != brushes.end()) return it->second;
        std::wstring name = target.substr(target.find_last_of(L"\\!") == std::wstring::npos ? 0 : target.find_last_of(L"\\!") + 1);
        if (name.size() > 4 && !_wcsicmp(name.c_str() + name.size() - 4, L".lnk")) name.resize(name.size() - 4);
        auto b = LetterIcon(e, dw, name, std::ceil(48 * e.scale * 1.5f));
        brushes.insert_or_assign(target, b);
        return b;
    };
    host.launch = [](std::wstring const&) {};
    host.closed = [] {};

    // Above where a dock would be (well clear of the real one: the capture never reaches below this).
    halo::LauncherAnchor anchor;
    anchor.centerX = (e.monitor.left + e.monitor.right) / 2;
    anchor.bottom = e.monitor.bottom - static_cast<int>(std::lround(200 * e.scale));
    anchor.dockWidth = 600 * e.scale;
    anchor.scale = e.scale;
    anchor.monitor = e.monitor;

    // The stand-in desktop: a window of ours behind the panel's whole area (and its shadow); the capture is cut from it.
    RECT back{std::max<LONG>(e.monitor.left, anchor.centerX - static_cast<LONG>(740 * e.scale)), std::max<LONG>(e.monitor.top, anchor.bottom - static_cast<LONG>(900 * e.scale)),
              std::min<LONG>(e.monitor.right, anchor.centerX + static_cast<LONG>(740 * e.scale)), anchor.bottom + static_cast<LONG>(60 * e.scale)};
    HWND backdrop = MakeBackdrop(back);
    Pump(After(Now(), 300));

    halo::Launcher launcher(GetModuleHandleW(nullptr), e.comp, e.gdev, std::move(host));
    Pump(After(Now(), 900));  // the prefetch worker delivers its snapshot
    launcher.Show(anchor);
    launcher.Raise();
    if (HWND ph = FindPanel(); ph) {
        Pump(After(Now(), holdMs > 1400 ? 1400 : holdMs));  // the 600 ms bind timer (notification feed), the 900 ms refresh (files)
        if (h2) launcher.TestCommand(24);                    // made-up radios, volume and brightness for the control column
        for (wchar_t c : query) PostMessageW(ph, WM_CHAR, c, 1);
        if (menu) launcher.TestCommand(2);
        Pump(After(Now(), 500));
        if (!hover.empty()) {  // the pointer over a control: the test points the panel logged give its position
            FILE* f = _wfopen((dir + L"\\launcher.log").c_str(), L"rb");
            std::string all;
            if (f) {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
                fclose(f);
            }
            const std::string key = Narrow(hover) + "=";
            int px = 0, py = 0;
            if (auto at = all.rfind(key); at != std::string::npos) std::sscanf(all.c_str() + at + key.size(), "%d,%d", &px, &py);
            if (px || py) {
                PostMessageW(ph, WM_MOUSEMOVE, 0, MAKELPARAM(px, py));
                Pump(After(Now(), 300));
                launcher.TestCommand(20);  // the hover plate at its end state
                launcher.TestCommand(29);
                if (press) PostMessageW(ph, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(px, py));
                Pump(After(Now(), 300));
            }
        }
        RECT panel{};
        GetWindowRect(ph, &panel);
        const LONG pad = static_cast<LONG>(24 * e.scale);
        RECT crop{std::max(panel.left - pad, back.left), std::max(panel.top - pad, back.top), std::min(panel.right + pad, back.right), std::min(panel.bottom + pad, back.bottom)};
        const bool ok = CaptureCrop(crop, out);
        std::printf("%s %ls (crop %ld,%ld..%ld,%ld, scale %.2f, panel %ld x %ld)\n", ok ? "captured" : "CAPTURE FAILED", out.c_str(), crop.left, crop.top, crop.right, crop.bottom,
                    e.scale, panel.right - panel.left, panel.bottom - panel.top);
    } else {
        std::printf("the panel window was not created\n");
    }
    launcher.Hide();
    Pump(After(Now(), 300));
    DestroyWindow(backdrop);
    std::printf("log: %ls\\launcher.log\n", dir.c_str());
    return 0;
}
