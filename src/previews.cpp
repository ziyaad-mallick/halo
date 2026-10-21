#include "theme.h"
#include "previews.h"

#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cmath>

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

constexpr float kPadOuter = 10, kGap = 8, kBoxW = 180, kBoxH = 112, kCardPad = 8, kTitleH = 20, kRise = 12;
constexpr float kCloseSize = 18;
constexpr int kInMs = 180, kOutMs = 120;
constexpr UINT_PTR kFadeTimer = 1, kRefreshTimer = 2;
constexpr int kMaxCards = 8;

Previews* g_self = nullptr;

void Log(std::wstring const& m) {
    auto line = L"[previews] " + m + L"\n";
    OutputDebugStringW(line.c_str());
    fputws(line.c_str(), stderr);
    fflush(stderr);
}

float EaseOut(float t) { return 1 - std::pow(1 - std::clamp(t, 0.0f, 1.0f), 3.0f); }

}  // namespace

Previews::Previews(HINSTANCE inst, wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev,
                   PreviewsHost host)
    : m_comp(comp), m_gdev(gdev), m_host(std::move(host)) {
    g_self = this;
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(m_dwrite.put())));
    m_easeOut = m_comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f});
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloPreviewsWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Halo Previews", WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, inst, this);
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

Previews::~Previews() {
    Clear();
    if (m_hwnd) DestroyWindow(m_hwnd);
    g_self = nullptr;
}

void Previews::Clear() {
    for (auto& c : m_cards)
        if (c.thumb) DwmUnregisterThumbnail(c.thumb);
    m_cards.clear();
}

bool Previews::ContainsCursor() const {
    if (!m_shown) return false;
    POINT pt;
    RECT r;
    return GetCursorPos(&pt) && GetWindowRect(m_hwnd, &r) && PtInRect(&r, pt);
}

void Previews::Raise() {
    if (m_shown) SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool Previews::Show(std::vector<HWND> const& windows, int centerX, int pillTop, RECT const& monitor, float scale) {
    const bool follow = m_shown;  // already up: move without replaying the entrance
    KillTimer(m_hwnd, kRefreshTimer);
    Clear();
    m_s = scale;
    const float s = scale;
    int maxCards = static_cast<int>((monitor.right - monitor.left - 4 * kPadOuter * s) / ((kBoxW + 2 * kCardPad + kGap) * s));
    maxCards = std::max(1, std::min(maxCards, kMaxCards));

    const float cardW = (kBoxW + 2 * kCardPad) * s, cardH = (kCardPad + kTitleH + 4 + kBoxH + kCardPad) * s;
    std::vector<Card> cards;
    for (HWND w : windows) {
        if (static_cast<int>(cards.size()) >= maxCards) break;
        if (!IsWindow(w)) continue;
        Card c{};
        c.src = w;
        wchar_t t[256] = {};
        GetWindowTextW(w, t, ARRAYSIZE(t));
        c.title = t;
        cards.push_back(std::move(c));
    }
    if (cards.empty()) {
        Hide(true);
        return false;
    }
    const int n = static_cast<int>(cards.size());
    m_w = 2 * kPadOuter * s + n * cardW + (n - 1) * kGap * s;
    m_h = cardH + 2 * kPadOuter * s;
    for (int i = 0; i < n; i++) {
        float x = kPadOuter * s + i * (cardW + kGap * s), y = kPadOuter * s;
        cards[i].card = {static_cast<LONG>(x), static_cast<LONG>(y), static_cast<LONG>(x + cardW), static_cast<LONG>(y + cardH)};
        SIZE src{};
        if (FAILED(DwmRegisterThumbnail(m_hwnd, cards[i].src, &cards[i].thumb))) cards[i].thumb = nullptr;
        if (cards[i].thumb && FAILED(DwmQueryThumbnailSourceSize(cards[i].thumb, &src))) src = {};
        if (src.cx <= 0 || src.cy <= 0) src = {static_cast<LONG>(kBoxW), static_cast<LONG>(kBoxH)};
        float bw = kBoxW * s, bh = kBoxH * s;
        float k = std::min(std::min(bw / src.cx, bh / src.cy), 2.0f);  // a tiny window isn't blown up past 2x
        float tw = src.cx * k, th = src.cy * k;
        float bx = x + kCardPad * s + (bw - tw) / 2, by = y + (kCardPad + kTitleH + 4) * s + (bh - th) / 2;
        cards[i].dst = {static_cast<LONG>(bx), static_cast<LONG>(by), static_cast<LONG>(bx + tw), static_cast<LONG>(by + th)};
    }
    m_cards = std::move(cards);

    // Visual tree: [strip: glass, plate, titles, close button]
    m_root.Children().RemoveAll();
    m_strip = m_comp.CreateContainerVisual();
    m_strip.Size({m_w, m_h});
    auto clip = m_comp.CreateRoundedRectangleGeometry();
    clip.CornerRadius({20 * s, 20 * s});
    clip.Size({m_w, m_h});
    m_strip.Clip(m_comp.CreateGeometricClip(clip));
    m_root.Children().InsertAtTop(m_strip);
    if (m_host.glass) {
        auto glass = m_host.glass(static_cast<int>(std::ceil(m_h)));
        glass.Size({m_w, m_h});
        m_strip.Children().InsertAtTop(glass);
    }
    m_plateShape = m_comp.CreateRoundedRectangleGeometry();
    m_plateShape.CornerRadius({12 * s, 12 * s});
    m_plateShape.Size({cardW, cardH});
    auto sprite = m_comp.CreateSpriteShape(m_plateShape);
    sprite.FillBrush(m_comp.CreateColorBrush(winrt::Windows::UI::Color{static_cast<uint8_t>(halo::theme::P().line.a*255+0.5f),static_cast<uint8_t>(halo::theme::P().line.r*255+0.5f),static_cast<uint8_t>(halo::theme::P().line.g*255+0.5f),static_cast<uint8_t>(halo::theme::P().line.b*255+0.5f)}));
    m_plate = m_comp.CreateShapeVisual();
    m_plate.Size({cardW, cardH});
    m_plate.Shapes().Append(sprite);
    m_plate.Opacity(0.0f);
    m_strip.Children().InsertAtTop(m_plate);

    auto makeSurface = [&](float w, float h) {
        return m_gdev.CreateDrawingSurface({std::ceil(w), std::ceil(h)},
                                           winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                           winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    };
    {  // Titles: one surface for the whole strip.
        auto surface = makeSurface(m_w, m_h);
        auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off));
        try {
            dc->Clear(D2D1::ColorF(0, 0.0f));
            winrt::com_ptr<IDWriteTextFormat> fmt;
            m_dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL, 12.0f * s, L"en-us", fmt.put());
            fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            winrt::com_ptr<IDWriteInlineObject> sign;
            m_dwrite->CreateEllipsisTrimmingSign(fmt.get(), sign.put());
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            fmt->SetTrimming(&trim, sign.get());
            winrt::com_ptr<ID2D1SolidColorBrush> brush;
            dc->CreateSolidColorBrush(halo::theme::P().text, brush.put());
            for (auto const& c : m_cards) {
                D2D1_RECT_F r = D2D1::RectF(off.x + c.card.left + kCardPad * s, off.y + c.card.top + kCardPad * s,
                                            off.x + c.card.right - (kCardPad + kCloseSize + 2) * s,
                                            off.y + c.card.top + (kCardPad + kTitleH) * s);
                dc->DrawText(c.title.c_str(), static_cast<UINT32>(c.title.size()), fmt.get(), r, brush.get(),
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
        v.Size({m_w, m_h});
        v.Brush(b);
        m_strip.Children().InsertAtTop(v);
    }
    {  // Close button: a disc with an x, moved to the hovered card.
        const float d = kCloseSize * s;
        auto surface = makeSurface(d, d);
        auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off));
        try {
            dc->Clear(D2D1::ColorF(0, 0.0f));
            winrt::com_ptr<ID2D1SolidColorBrush> disc, ink;
            dc->CreateSolidColorBrush(halo::theme::A(halo::theme::P().text, 0.15f), disc.put());
            dc->CreateSolidColorBrush(halo::theme::P().text, ink.put());
            float cx = off.x + d / 2, cy = off.y + d / 2, a = d * 0.28f;
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), d / 2, d / 2), disc.get());
            dc->DrawLine({cx - a, cy - a}, {cx + a, cy + a}, ink.get(), 1.4f * s);
            dc->DrawLine({cx - a, cy + a}, {cx + a, cy - a}, ink.get(), 1.4f * s);
        } catch (...) {
            interop->EndDraw();
            throw;
        }
        winrt::check_hresult(interop->EndDraw());
        auto b = m_comp.CreateSurfaceBrush(surface);
        b.Stretch(wuc::CompositionStretch::None);
        m_closeBtn = m_comp.CreateSpriteVisual();
        m_closeBtn.Size({d, d});
        m_closeBtn.Brush(b);
        m_closeBtn.Opacity(0.0f);
        m_strip.Children().InsertAtTop(m_closeBtn);
    }
    m_hover = -1;

    int x = std::clamp(centerX - static_cast<int>(m_w) / 2, static_cast<int>(monitor.left + 8 * s),
                       std::max(static_cast<int>(monitor.left), static_cast<int>(monitor.right - m_w - 8 * s)));
    int top = pillTop - static_cast<int>(m_h) - static_cast<int>(8 * s);
    int winH = static_cast<int>(m_h);  // the rise starts below the window and is clipped by its edge
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, top, static_cast<int>(m_w), winH, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    m_shown = true;
    if (follow) {
        m_p = 1.0f;
        KillTimer(m_hwnd, kFadeTimer);
        ApplyThumbs(1.0f);
    } else {
        m_p = 0.0f;
        m_fadeIn = true;
        m_fadeStart = std::chrono::steady_clock::now();
        m_strip.Opacity(0.0f);
        m_strip.Offset({0, kRise * s, 0});
        auto fade = m_comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, 1.0f, m_easeOut);
        fade.Duration(std::chrono::milliseconds(kInMs));
        m_strip.StartAnimation(L"Opacity", fade);
        auto rise = m_comp.CreateScalarKeyFrameAnimation();
        rise.InsertKeyFrame(1.0f, 0.0f, m_easeOut);
        rise.Duration(std::chrono::milliseconds(kInMs));
        m_strip.StartAnimation(L"Offset.Y", rise);
        ApplyThumbs(0.0f);
        SetTimer(m_hwnd, kFadeTimer, 15, nullptr);
    }
    Log(L"show " + std::to_wstring(n) + L" cards at x=" + std::to_wstring(x) + L" y=" + std::to_wstring(top) +
        (follow ? L" (follow)" : L""));
    return true;
}

void Previews::ApplyThumbs(float p) {
    const float e = EaseOut(p);
    const int dy = static_cast<int>((1 - e) * kRise * m_s);
    for (auto& c : m_cards) {
        if (!c.thumb) continue;
        DWM_THUMBNAIL_PROPERTIES props{};
        props.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_OPACITY | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY;
        props.rcDestination = {c.dst.left, c.dst.top + dy, c.dst.right, c.dst.bottom + dy};
        props.opacity = static_cast<BYTE>(std::clamp(e, 0.0f, 1.0f) * 255);
        props.fVisible = TRUE;
        props.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(c.thumb, &props);
    }
}

void Previews::Hide(bool instant) {
    if (!m_shown) return;
    m_shown = false;
    KillTimer(m_hwnd, kRefreshTimer);
    if (instant) {
        Finish();
        return;
    }
    m_fadeIn = false;
    m_fadeStart = std::chrono::steady_clock::now();
    auto fade = m_comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, 0.0f, m_easeOut);
    fade.Duration(std::chrono::milliseconds(kOutMs));
    m_strip.StartAnimation(L"Opacity", fade);
    auto drop = m_comp.CreateScalarKeyFrameAnimation();
    drop.InsertKeyFrame(1.0f, kRise * m_s, m_easeOut);
    drop.Duration(std::chrono::milliseconds(kOutMs));
    m_strip.StartAnimation(L"Offset.Y", drop);
    SetTimer(m_hwnd, kFadeTimer, 15, nullptr);
}

// After the fade out (or at once): thumbnails off, window hidden.
void Previews::Finish() {
    KillTimer(m_hwnd, kFadeTimer);
    Clear();
    ShowWindow(m_hwnd, SW_HIDE);
    m_root.Children().RemoveAll();
    m_strip = nullptr;
    m_plate = nullptr;
    m_plateShape = nullptr;
    m_closeBtn = nullptr;
    m_hover = -1;
    m_inside = false;
    m_tracking = false;
    Log(L"hidden");
}

void Previews::SetHover(int index) {
    if (index == m_hover) return;
    m_hover = index;
    const float s = m_s;
    auto anim = [&](wuc::CompositionObject const& o, float to) {
        auto a = m_comp.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(1.0f, to, m_easeOut);
        a.Duration(std::chrono::milliseconds(120));
        o.StartAnimation(L"Opacity", a);
    };
    if (index < 0) {
        anim(m_plate, 0.0f);
        anim(m_closeBtn, 0.0f);
        return;
    }
    auto const& c = m_cards[index];
    m_plate.Offset({static_cast<float>(c.card.left), static_cast<float>(c.card.top), 0});
    m_closeBtn.Offset({c.card.right - (kCardPad + kCloseSize) * s, c.card.top + (kCardPad + (kTitleH - kCloseSize) / 2) * s, 0});
    anim(m_plate, 1.0f);
    anim(m_closeBtn, 1.0f);
}

int Previews::CardAt(int x, int y) const {
    POINT p{x, y};
    for (size_t i = 0; i < m_cards.size(); i++)
        if (PtInRect(&m_cards[i].card, p)) return static_cast<int>(i);
    return -1;
}

bool Previews::OnClose(int index, int x, int y) const {
    if (index < 0) return false;
    auto const& c = m_cards[index];
    const float d = kCloseSize * m_s;
    float l = c.card.right - kCardPad * m_s - d, t = c.card.top + (kCardPad + (kTitleH - kCloseSize) / 2) * m_s;
    return x >= l - 3 && x <= l + d + 3 && y >= t - 3 && y <= t + d + 3;
}

LRESULT CALLBACK Previews::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_self && g_self->m_hwnd) return g_self->OnMessage(m, w, l);  // not during CreateWindowEx
    return DefWindowProcW(h, m, w, l);
}

LRESULT Previews::OnMessage(UINT m, WPARAM w, LPARAM l) {
    const int mx = static_cast<short>(LOWORD(l)), my = static_cast<short>(HIWORD(l));
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!m_tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
                m_tracking = TrackMouseEvent(&tme);
            }
            if (!m_inside) {
                m_inside = true;
                if (m_host.entered) m_host.entered();
            }
            if (m_shown) SetHover(CardAt(mx, my));
            return 0;
        case WM_MOUSELEAVE:
            m_tracking = false;
            m_inside = false;
            if (m_shown) SetHover(-1);
            if (m_host.left) m_host.left();
            return 0;
        case WM_LBUTTONUP: {
            if (!m_shown) return 0;
            int i = CardAt(mx, my);
            if (i < 0) return 0;
            HWND src = m_cards[i].src;
            if (OnClose(i, mx, my)) {
                PostMessageW(src, WM_CLOSE, 0, 0);
                SetTimer(m_hwnd, kRefreshTimer, 350, nullptr);  // let it go, then re-list
            } else if (m_host.activate) {
                m_host.activate(src);
            }
            return 0;
        }
        case WM_TIMER:
            if (w == kRefreshTimer) {
                KillTimer(m_hwnd, kRefreshTimer);
                if (m_shown && m_host.refresh) m_host.refresh();
            } else if (w == kFadeTimer) {
                float t = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m_fadeStart).count();
                if (m_fadeIn) {
                    m_p = std::min(1.0f, t / kInMs);
                    ApplyThumbs(m_p);
                    if (m_p >= 1.0f) KillTimer(m_hwnd, kFadeTimer);
                } else if (t >= kOutMs) {
                    Finish();
                } else {
                    ApplyThumbs(std::max(0.0f, 1.0f - t / kOutMs));
                }
            }
            return 0;
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

}  // namespace halo
