#include "menu.h"
#include "theme.h"

#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

struct ICompositorDesktopInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopWindowTarget(HWND hwnd, BOOL isTopmost, void** target) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnsureOnThread(DWORD threadId) = 0;
};
__CRT_UUID_DECL(ICompositorDesktopInterop, 0x29E691FA, 0x4567, 0x4DCA, 0xB3, 0x19, 0xD0, 0xF2, 0x07, 0xEB, 0x68, 0x07)
#ifndef HALO_COMPOSITION_INTEROP_DEFINED
#define HALO_COMPOSITION_INTEROP_DEFINED
struct ICompositionDrawingSurfaceInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginDraw(RECT const* updateRect, REFIID iid, void** updateObject, POINT* offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resize(SIZE size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Scroll(RECT const* scrollRect, RECT const* clipRect, int dx, int dy) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendDraw() = 0;
};
__CRT_UUID_DECL(ICompositionDrawingSurfaceInterop, 0xFD04E6E3, 0xFE0C, 0x4C3C, 0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE)
#endif

namespace halo {
namespace {

// Layout, logical px.
constexpr float kRadius = 14, kPad = 6, kRowH = 28, kSepH = 9, kTextPad = 12, kHoverRadius = 8, kGap = 8;
constexpr float kMinW = 190, kMaxW = 340, kRise = 6;
constexpr int kInMs = 120, kOutMs = 90;

Menu* g_menu = nullptr;  // WinEvent callbacks carry no context

void Log(std::wstring const& m) {
    auto line = L"[halo] menu: " + m + L"\n";
    OutputDebugStringW(line.c_str());
    fputws(line.c_str(), stderr);
    fflush(stderr);
}

std::wstring RectStr(RECT const& r) {
    return std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," + std::to_wstring(r.right) + L"," + std::to_wstring(r.bottom);
}

}  // namespace

void CALLBACK MenuForegroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (!g_menu || !g_menu->m_open || !hwnd) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) return;
    Log(L"foreground moved, closing");
    g_menu->Close();
}

Menu::Menu(HINSTANCE inst, wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev, MenuHost host)
    : m_comp(comp), m_gdev(gdev), m_host(std::move(host)) {
    g_menu = this;
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(m_dwrite.put())));
    m_easeOut = m_comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f});
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloMenuWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Halo Menu", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, inst, this);
    if (!m_hwnd) throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
    BOOL on = TRUE;
    DwmSetWindowAttribute(m_hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));
    DWORD policy = 2;  // DWMNCRP_DISABLED: no system shadow around the window's rectangle
    DwmSetWindowAttribute(m_hwnd, 2 /* DWMWA_NCRENDERING_POLICY */, &policy, sizeof(policy));
    DWORD corners = 1;  // DWMWCP_DONOTROUND
    DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
    auto interop = m_comp.as<ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(m_hwnd, TRUE, winrt::put_abi(m_target)));
    m_root = m_comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment({1, 1});
    m_target.Root(m_root);
}

Menu::~Menu() {
    if (m_hook) UnhookWinEvent(m_hook);
    if (m_hwnd) DestroyWindow(m_hwnd);
    g_menu = nullptr;
}

void Menu::Raise() {
    if (m_open) SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void Menu::Show(std::vector<MenuEntry> entries, int centerX, int pillTop, RECT const& monitor, float scale) {
    if (entries.empty()) return;
    const bool wasOpen = m_open;
    ++m_gen;
    m_s = scale;
    const float s = scale;
    m_entries = std::move(entries);

    // Measure: the widest label sets the width, within bounds.
    winrt::com_ptr<IDWriteTextFormat> fmt;
    winrt::check_hresult(m_dwrite->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f * s,
                                                    L"en-us", fmt.put()));
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    winrt::com_ptr<IDWriteInlineObject> sign;
    m_dwrite->CreateEllipsisTrimmingSign(fmt.get(), sign.put());
    DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    fmt->SetTrimming(&trim, sign.get());
    float widest = 0;
    for (auto const& e : m_entries) {
        if (e.separator) continue;
        winrt::com_ptr<IDWriteTextLayout> layout;
        if (SUCCEEDED(m_dwrite->CreateTextLayout(e.label.c_str(), static_cast<UINT32>(e.label.size()), fmt.get(), 4000.0f,
                                                 100.0f, layout.put()))) {
            DWRITE_TEXT_METRICS tm{};
            layout->GetMetrics(&tm);
            widest = std::max(widest, tm.widthIncludingTrailingWhitespace);
        }
    }
    m_w = static_cast<int>(std::round(std::clamp(widest + 2 * (kPad + kTextPad) * s, kMinW * s, kMaxW * s)));
    m_rows.clear();
    float y = kPad * s;
    for (auto const& e : m_entries) {
        float h = (e.separator ? kSepH : kRowH) * s;
        RECT r{static_cast<LONG>(kPad * s), static_cast<LONG>(std::round(y)), static_cast<LONG>(m_w - kPad * s),
               static_cast<LONG>(std::round(y + h))};
        m_rows.push_back({r, e.separator, e.label});
        y += h;
    }
    m_h = static_cast<int>(std::round(y + kPad * s));

    // Visual tree: [menu: glass, hover plate, text]
    m_root.Children().RemoveAll();
    m_menu = m_comp.CreateContainerVisual();
    m_menu.Size({static_cast<float>(m_w), static_cast<float>(m_h)});
    auto clip = m_comp.CreateRoundedRectangleGeometry();
    clip.CornerRadius({kRadius * s, kRadius * s});
    clip.Size({static_cast<float>(m_w), static_cast<float>(m_h)});
    m_menu.Clip(m_comp.CreateGeometricClip(clip));
    m_root.Children().InsertAtTop(m_menu);
    if (m_host.glass) {
        auto glass = m_host.glass(m_h, kRadius * s);
        glass.Size({static_cast<float>(m_w), static_cast<float>(m_h)});
        m_menu.Children().InsertAtTop(glass);
    }
    auto const& pal = theme::P();
    {  // The hover pill, moved to the row under the pointer.
        auto geo = m_comp.CreateRoundedRectangleGeometry();
        geo.CornerRadius({kHoverRadius * s, kHoverRadius * s});
        geo.Size({static_cast<float>(m_w - 2 * kPad * s), kRowH * s});
        auto shape = m_comp.CreateSpriteShape(geo);
        auto col = pal.fillHover;
        shape.FillBrush(m_comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(
            static_cast<uint8_t>(col.a * 255 + 0.5f), static_cast<uint8_t>(col.r * 255 + 0.5f),
            static_cast<uint8_t>(col.g * 255 + 0.5f), static_cast<uint8_t>(col.b * 255 + 0.5f))));
        m_hoverPlate = m_comp.CreateShapeVisual();
        m_hoverPlate.Size({static_cast<float>(m_w), static_cast<float>(m_h)});
        m_hoverPlate.Shapes().Append(shape);
        m_hoverPlate.Opacity(0.0f);
        m_menu.Children().InsertAtTop(m_hoverPlate);
    }
    {  // Labels and separators: one surface.
        auto surface = m_gdev.CreateDrawingSurface({static_cast<float>(m_w), static_cast<float>(m_h)},
                                                   winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                   winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off));
        try {
            dc->Clear(D2D1::ColorF(0, 0.0f));
            dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
            winrt::com_ptr<ID2D1SolidColorBrush> ink, line;
            dc->CreateSolidColorBrush(pal.text, ink.put());
            dc->CreateSolidColorBrush(pal.line, line.put());
            for (auto const& row : m_rows) {
                if (row.separator) {
                    float my = off.y + (row.r.top + row.r.bottom) / 2.0f;
                    dc->FillRectangle(D2D1::RectF(off.x + row.r.left + kTextPad * s / 2, my, off.x + row.r.right - kTextPad * s / 2,
                                                  my + std::max(1.0f, std::round(s))),
                                      line.get());
                    continue;
                }
                D2D1_RECT_F tr = D2D1::RectF(off.x + row.r.left + kTextPad * s, off.y + row.r.top, off.x + row.r.right - kTextPad * s,
                                             off.y + row.r.bottom);
                dc->DrawText(row.label.c_str(), static_cast<UINT32>(row.label.size()), fmt.get(), tr, ink.get(),
                             D2D1_DRAW_TEXT_OPTIONS_CLIP);
            }
        } catch (...) {
            interop->EndDraw();
            throw;
        }
        winrt::check_hresult(interop->EndDraw());
        auto b = m_comp.CreateSurfaceBrush(surface);
        b.Stretch(wuc::CompositionStretch::None);
        auto v = m_comp.CreateSpriteVisual();
        v.Size({static_cast<float>(m_w), static_cast<float>(m_h)});
        v.Brush(b);
        m_menu.Children().InsertAtTop(v);
    }
    m_hover = -1;

    // Centered above the icon, `kGap` above the pill, kept on the monitor.
    int x = std::clamp(centerX - m_w / 2, static_cast<int>(monitor.left + 8 * s),
                       std::max(static_cast<int>(monitor.left), static_cast<int>(monitor.right - m_w - 8 * s)));
    int top = std::max(static_cast<int>(monitor.top + 8 * s), pillTop - m_h - static_cast<int>(std::round(kGap * s)));
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, top, m_w, m_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    if (!wasOpen) {
        m_menu.Opacity(0.0f);
        m_menu.Offset({0, kRise * s, 0});
        auto fade = m_comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, 1.0f, m_easeOut);
        fade.Duration(std::chrono::milliseconds(kInMs));
        m_menu.StartAnimation(L"Opacity", fade);
        auto rise = m_comp.CreateScalarKeyFrameAnimation();
        rise.InsertKeyFrame(1.0f, 0.0f, m_easeOut);
        rise.Duration(std::chrono::milliseconds(kInMs));
        m_menu.StartAnimation(L"Offset.Y", rise);
    }
    m_open = true;
    if (!m_hook) m_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, MenuForegroundProc, 0, 0,
                                          WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetCapture(m_hwnd);  // a click anywhere outside the menu arrives here and closes it
    Log(L"open at " + RectStr({x, top, x + m_w, top + m_h}) + L", " + std::to_wstring(m_rows.size()) + L" rows, capture=" +
        (GetCapture() == m_hwnd ? L"yes" : L"NO"));
    for (size_t i = 0; i < m_rows.size(); i++) {
        RECT sr{x + m_rows[i].r.left, top + m_rows[i].r.top, x + m_rows[i].r.right, top + m_rows[i].r.bottom};
        Log(L"row " + std::to_wstring(i) + (m_rows[i].separator ? L" ---" : L" '" + m_rows[i].label + L"'") + L" rect=" + RectStr(sr));
    }
    if (!wasOpen && m_host.opened) m_host.opened();
}

void Menu::Close(bool instant) {
    if (!m_open) return;
    m_open = false;
    if (GetCapture() == m_hwnd) ReleaseCapture();
    if (m_hook) {
        UnhookWinEvent(m_hook);
        m_hook = nullptr;
    }
    Log(L"close");
    if (instant) {
        Finish();
    } else {
        unsigned gen = ++m_gen;
        auto batch = m_comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
        auto fade = m_comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, 0.0f, m_easeOut);
        fade.Duration(std::chrono::milliseconds(kOutMs));
        m_menu.StartAnimation(L"Opacity", fade);
        batch.End();
        batch.Completed([this, gen](auto&&, auto&&) {
            if (gen == m_gen && !m_open) Finish();
        });
    }
    if (m_host.closed) m_host.closed();
}

void Menu::Finish() {
    ShowWindow(m_hwnd, SW_HIDE);
    m_root.Children().RemoveAll();
    m_menu = nullptr;
    m_hoverPlate = nullptr;
    m_rows.clear();
    m_hover = -1;
}

int Menu::RowAt(int x, int y) const {
    POINT p{x, y};
    for (size_t i = 0; i < m_rows.size(); i++) {
        if (m_rows[i].separator) continue;
        RECT r = m_rows[i].r;
        r.left = 0;  // the pill's margin still counts as the row
        r.right = m_w;
        if (PtInRect(&r, p)) return static_cast<int>(i);
    }
    return -1;
}

void Menu::SetHover(int row) {
    if (row == m_hover || !m_open) return;
    m_hover = row;
    if (row < 0) {
        m_hoverPlate.Opacity(0.0f);
        return;
    }
    m_hoverPlate.Offset({static_cast<float>(m_rows[row].r.left), static_cast<float>(m_rows[row].r.top), 0});
    m_hoverPlate.Opacity(1.0f);
    Log(L"hover row " + std::to_wstring(row) + L" '" + m_rows[row].label + L"'");
}

void Menu::Choose(int row) {
    if (!m_open || row < 0 || row >= static_cast<int>(m_entries.size()) || m_entries[row].separator) return;
    auto action = std::move(m_entries[row].action);
    Log(L"choose row " + std::to_wstring(row) + L" '" + m_entries[row].label + L"'");
    Close();
    if (action) action();
}

void Menu::TestHover(int row) { SetHover(row); }
void Menu::TestActivate(int row) { Choose(row); }

LRESULT CALLBACK Menu::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Menu*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->m_hwnd) return self->OnMessage(m, w, l);  // not during CreateWindowEx
    return DefWindowProcW(h, m, w, l);
}

LRESULT Menu::OnMessage(UINT m, WPARAM w, LPARAM l) {
    const int mx = static_cast<short>(LOWORD(l)), my = static_cast<short>(HIWORD(l));
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (m_open) SetHover(RowAt(mx, my));
            return 0;
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            if (!m_open) return 0;
            POINT p{mx, my};
            RECT client{0, 0, m_w, m_h};
            if (!PtInRect(&client, p)) {
                Log(L"click outside");
                Close();
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (m_open) Choose(RowAt(mx, my));
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) Close();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && m_open) Close();
            return 0;
        case WM_CAPTURECHANGED:
            if (m_open && reinterpret_cast<HWND>(l) != m_hwnd) {
                Log(L"capture lost");
                Close();
            }
            return 0;
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

}  // namespace halo
