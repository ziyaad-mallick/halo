#include "theme.h"
#include "tokens.h"
#include "cluster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.Composition.h>
#include <chrono>
#include <winrt/Windows.Foundation.Collections.h>

#ifndef __ICompositionDrawingSurfaceInterop_INTERFACE_DEFINED__
#define __ICompositionDrawingSurfaceInterop_INTERFACE_DEFINED__
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

namespace {

// Power setting GUIDs
static constexpr GUID kGuidAcdcPowerSource = {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};
static constexpr GUID kGuidBatteryPercentageRemaining = {0xa7ad8041, 0xb45a, 0x4cae, {0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1}};

// Segoe Fluent Icons; Windows 10 doesn't ship it (CreateTextFormat succeeds anyway and draws the glyph codes as
// boxes in a fallback font), but its predecessor Segoe MDL2 Assets does.
wchar_t const* IconFontFamily(IDWriteFactory* factory) {
    winrt::com_ptr<IDWriteFontCollection> fonts;
    UINT32 at = 0;
    BOOL found = TRUE;
    if (SUCCEEDED(factory->GetSystemFontCollection(fonts.put())) &&
        SUCCEEDED(fonts->FindFamilyName(L"Segoe Fluent Icons", &at, &found)) && !found)
        return L"Segoe MDL2 Assets";
    return L"Segoe Fluent Icons";
}

// Internet reachability and whether the link is wired (Ethernet) rather than Wi-Fi / mobile.
struct NetState {
    bool online = false, wired = false;
};
NetState ReadNet() {
    namespace conn = winrt::Windows::Networking::Connectivity;
    NetState n;
    try {
        if (auto profile = conn::NetworkInformation::GetInternetConnectionProfile()) {
            n.online = profile.GetNetworkConnectivityLevel() == conn::NetworkConnectivityLevel::InternetAccess;
            n.wired = !profile.IsWlanConnectionProfile() && !profile.IsWwanConnectionProfile();
        }
    } catch (...) {}
    if (!n.online) n.wired = false;
    return n;
}

}  // namespace

DockCluster::DockCluster(winrt::Windows::UI::Composition::Compositor const& compositor,
                         winrt::Windows::UI::Composition::CompositionGraphicsDevice const& gdev,
                         HWND hwnd,
                         float pillHeight,
                         float scale)
    : m_compositor(compositor), m_gdev(gdev), m_hwnd(hwnd), m_pillHeight(pillHeight), m_scale(scale) {
    InitDirectWrite();
    Layout();

    if (m_gdev) {
        m_surface = m_gdev.CreateDrawingSurface(
            {m_width, m_pillHeight},
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);

        m_visual = m_compositor.CreateSpriteVisual();
        m_visual.Size({m_width, m_pillHeight});
        auto brush = m_compositor.CreateSurfaceBrush(m_surface);
        m_visual.Brush(brush);
    }

    RegisterPowerNotifications();
    RegisterNetworkCallback();
    SetupClockTimer();
    Render();
}

DockCluster::~DockCluster() {
    if (m_hwnd) {
        KillTimer(m_hwnd, kClockTimer);
    }
    UnregisterPowerNotifications();
    UnregisterNetworkCallback();
}

void DockCluster::InitDirectWrite() {
    winrt::check_hresult(DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwriteFactory.put())));

    // 13pt tabular figures in Segoe UI Variable Text for clock
    const float timePtSize = 13.0f * (96.0f / 72.0f) * m_scale;
    winrt::check_hresult(m_dwriteFactory->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, timePtSize,
        L"en-us", m_tfTime.put()));

    // Tabular figures typography
    if (SUCCEEDED(m_dwriteFactory->CreateTypography(m_typography.put()))) {
        DWRITE_FONT_FEATURE feat{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
        m_typography->AddFontFeature(feat);
    }

    // Segoe Fluent Icons for Wi-Fi and Battery.
    const float iconSize = 14.0f * m_scale;
    wchar_t const* iconFont = IconFontFamily(m_dwriteFactory.get());
    winrt::check_hresult(m_dwriteFactory->CreateTextFormat(
        iconFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, iconSize,
        L"en-us", m_tfIcons.put()));

    // 11pt Segoe UI Variable Text for battery percentage
    const float battTextSize = 11.0f * (96.0f / 72.0f) * m_scale;
    winrt::check_hresult(m_dwriteFactory->CreateTextFormat(
        L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, battTextSize,
        L"en-us", m_tfBattText.put()));
}

void DockCluster::SetupClockTimer() {
    if (!m_hwnd) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    // Milliseconds to next :00 minute boundary with 25 ms margin
    DWORD ms = (60 - st.wSecond) * 1000 - st.wMilliseconds + 25;
    if (ms < 50) ms = 60000;
    SetTimer(m_hwnd, kClockTimer, ms, nullptr);
}

void DockCluster::RegisterPowerNotifications() {
    if (!m_hwnd) return;
    m_powerNotifyBatt = RegisterPowerSettingNotification(
        m_hwnd, &kGuidBatteryPercentageRemaining, DEVICE_NOTIFY_WINDOW_HANDLE);
    m_powerNotifyAcDc = RegisterPowerSettingNotification(
        m_hwnd, &kGuidAcdcPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);
}

void DockCluster::UnregisterPowerNotifications() {
    if (m_powerNotifyBatt) {
        UnregisterPowerSettingNotification(m_powerNotifyBatt);
        m_powerNotifyBatt = nullptr;
    }
    if (m_powerNotifyAcDc) {
        UnregisterPowerSettingNotification(m_powerNotifyAcDc);
        m_powerNotifyAcDc = nullptr;
    }
}

void DockCluster::RegisterNetworkCallback() {
    try {
        m_networkToken = winrt::Windows::Networking::Connectivity::NetworkInformation::NetworkStatusChanged(
            [hwnd = m_hwnd](auto const&) {
                if (hwnd) PostMessageW(hwnd, kMsgNetworkChanged, 0, 0);
            });
        m_networkRegistered = true;
    } catch (...) {
        m_networkRegistered = false;
    }
}

void DockCluster::UnregisterNetworkCallback() {
    if (m_networkRegistered) {
        try {
            winrt::Windows::Networking::Connectivity::NetworkInformation::NetworkStatusChanged(m_networkToken);
        } catch (...) {}
        m_networkRegistered = false;
    }
}

bool DockCluster::HitTestStart(float localX, float localY) const {
    return localX >= m_startX && localX <= (m_startX + m_startWidth) &&
           localY >= 0.0f && localY <= m_pillHeight;
}

bool DockCluster::HitTestCluster(float localX, float localY) const {
    return localX >= 0.0f && localX <= m_width && localY >= 0.0f && localY <= m_pillHeight;
}

void DockCluster::OnTimer(UINT_PTR timerId) {
    if (timerId == kClockTimer) {
        KillTimer(m_hwnd, kClockTimer);
        SetupClockTimer();
        Render();
    }
}

void DockCluster::OnTimeChange() {
    if (m_hwnd) KillTimer(m_hwnd, kClockTimer);
    SetupClockTimer();
    Render();
}

void DockCluster::OnSettingChange() {
    if (m_hwnd) KillTimer(m_hwnd, kClockTimer);
    SetupClockTimer();
    Render();
}

void DockCluster::OnPowerBroadcast(WPARAM w, LPARAM l) {
    if (w == PBT_POWERSETTINGCHANGE || w == PBT_APMPOWERSTATUSCHANGE) {
        Render();
    }
}

void DockCluster::OnNetworkChanged() {
    Render();
}

// A layout of `text` in `format`, vertically centered in the pill.
winrt::com_ptr<IDWriteTextLayout> DockCluster::MakeLayout(std::wstring const& text, IDWriteTextFormat* format,
                                                         float width) const {
    winrt::com_ptr<IDWriteTextLayout> layout;
    winrt::check_hresult(m_dwriteFactory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format,
                                                           width, m_pillHeight, layout.put()));
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (m_typography) layout->SetTypography(m_typography.get(), {0, static_cast<UINT32>(text.size())});
    return layout;
}

float DockCluster::TextWidth(std::wstring const& text, IDWriteTextFormat* format) const {
    DWRITE_TEXT_METRICS tm{};
    MakeLayout(text, format, 1000.0f)->GetMetrics(&tm);
    return std::ceil(tm.widthIncludingTrailingWhitespace);
}

// Fixed slots sized for the widest content, so the dock never changes width
// as the time or battery level ticks over: time | Wi-Fi | battery % | Start.
void DockCluster::Layout() {
    const float s = m_scale;
    m_timeX = 4.0f * s;
    m_timeW = TextWidth(L"88:88", m_tfTime.get());
    m_wifiX = m_timeX + m_timeW + 10.0f * s;
    m_iconW = std::max(TextWidth(L"\uE701", m_tfIcons.get()), TextWidth(L"\uEBAA", m_tfIcons.get()));
    m_battX = m_wifiX + m_iconW + 8.0f * s;
    m_battTextW = TextWidth(L"100%", m_tfBattText.get());
    m_startWidth = std::round((halo::settings::Get().halo2 ? 36.0f : 28.0f) * s);  // one rest-size icon slot
    m_startX = m_battX + m_iconW + 3.0f * s + m_battTextW + 8.0f * s;
    m_width = std::ceil(m_startX + m_startWidth);
}

void DockCluster::Render() {
    if (!m_surface || !m_dwriteFactory) return;

    auto interop = m_surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset))) return;

    try {
        const float ox = static_cast<float>(offset.x), oy = static_cast<float>(offset.y);
        dc->Clear(D2D1::ColorF(0, 0.0f));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        winrt::com_ptr<ID2D1SolidColorBrush> white, dim;
        dc->CreateSolidColorBrush(halo::theme::P().text, white.put());
        dc->CreateSolidColorBrush(halo::theme::A(halo::theme::P().dim, 0.7f), dim.put());

        // Time, in the user's 12/24-hour setting, centered in its slot.
        SYSTEMTIME st;
        GetLocalTime(&st);
        int is24 = 0;
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_ITIME | LOCALE_RETURN_NUMBER,
                        reinterpret_cast<LPWSTR>(&is24), sizeof(is24) / sizeof(wchar_t));
        int hour = is24 ? st.wHour : (st.wHour % 12 == 0 ? 12 : st.wHour % 12);
        wchar_t timeBuf[16];
        swprintf(timeBuf, 16, is24 ? L"%02d:%02d" : L"%d:%02d", hour, st.wMinute);
        auto time = MakeLayout(timeBuf, m_tfTime.get(), m_timeW);
        time->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        dc->DrawTextLayout({ox + m_timeX, oy}, time.get(), white.get());

        {
        // Network: Wi-Fi or Ethernet glyph; dimmed Wi-Fi when offline.
        const NetState ns = ReadNet();
        const bool online = ns.online;
        const wchar_t net = ns.wired ? L'\uE839' : L'\uE701';
        auto netLayout = MakeLayout(std::wstring(1, net), m_tfIcons.get(), m_iconW);
        netLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        dc->DrawTextLayout({ox + m_wifiX, oy}, netLayout.get(), online ? white.get() : dim.get());

        }

        // Battery: Battery0..10 (EBA0..EBAA) or BatteryCharging0..10 (EBAB..EBB5), then the percentage.
        SYSTEM_POWER_STATUS sps{};
        if (GetSystemPowerStatus(&sps) && !(sps.BatteryFlag & 128) && sps.BatteryLifePercent <= 100) {
            const int pct = sps.BatteryLifePercent;
            const int level = std::clamp((pct + 5) / 10, 0, 10);
            const wchar_t glyph = static_cast<wchar_t>((sps.ACLineStatus == 1 ? 0xEBAB : 0xEBA0) + level);
            auto batt = MakeLayout(std::wstring(1, glyph), m_tfIcons.get(), m_iconW);
            batt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            dc->DrawTextLayout({ox + m_battX, oy}, batt.get(), white.get());
            wchar_t pctBuf[8];
            swprintf(pctBuf, 8, L"%d%%", pct);
            auto pctLayout = MakeLayout(pctBuf, m_tfBattText.get(), m_battTextW);
            dc->DrawTextLayout({ox + m_battX + m_iconW + 3.0f * m_scale, oy}, pctLayout.get(), white.get());
        }

        // Start: the Windows 11 logo, four panes under one blue gradient.
        const float logo = std::round((halo::settings::Get().halo2 ? 28.0f : 20.0f) * m_scale);  // 17 before: ~18 % larger
        const float gap = std::max(1.0f, std::round(logo / 14.0f));
        const float pane = (logo - gap) / 2;
        const float lx = ox + m_startX + (m_startWidth - logo) / 2;
        const float ly = oy + std::round((m_pillHeight - logo) / 2);
        D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(0x5AD0FF)}, {1.0f, D2D1::ColorF(0x0A6CDA)}};
        winrt::com_ptr<ID2D1GradientStopCollection> stopColl;
        winrt::com_ptr<ID2D1LinearGradientBrush> blue;
        dc->CreateGradientStopCollection(stops, 2, stopColl.put());
        dc->CreateLinearGradientBrush({{lx, ly}, {lx + logo, ly + logo}}, stopColl.get(), blue.put());
        const float r = std::max(0.5f, pane / 10.0f);
        for (int row = 0; row < 2; row++)
            for (int col = 0; col < 2; col++) {
                const float x = lx + col * (pane + gap), y = ly + row * (pane + gap);
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + pane, y + pane), r, r), blue.get());
            }
    } catch (...) {
        interop->EndDraw();
        throw;
    }
    interop->EndDraw();
}

// ---- DockWidgets: the Now Playing pill (HALO 2.0) -----------------------------------------------

namespace {

using halo::ui::kBody;
using halo::ui::kCaption;
using halo::ui::kHoverMs;
using halo::ui::kIcon;
using halo::ui::kRadiusSmall;
using halo::ui::kS2;
using halo::ui::kS3;
using halo::ui::kSlowMs;

// The artwork box-filtered to px x px (premultiplied BGRA in, out), so the pill keeps a few KB rather than the source image.
std::vector<uint32_t> Thumb(std::vector<uint32_t> const& src, int srcPx, int px) {
    std::vector<uint32_t> out(static_cast<size_t>(px) * px, 0);
    if (srcPx <= 0 || src.size() < static_cast<size_t>(srcPx) * srcPx) return out;
    for (int y = 0; y < px; y++)
        for (int x = 0; x < px; x++) {
            const int x0 = x * srcPx / px, x1 = std::max(x0 + 1, (x + 1) * srcPx / px);
            const int y0 = y * srcPx / px, y1 = std::max(y0 + 1, (y + 1) * srcPx / px);
            uint32_t acc[4] = {};
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    const uint32_t c = src[static_cast<size_t>(yy) * srcPx + xx];
                    for (int k = 0; k < 4; k++) acc[k] += (c >> (8 * k)) & 0xFF;
                }
            const uint32_t n = static_cast<uint32_t>((x1 - x0) * (y1 - y0));
            uint32_t c = 0;
            for (int k = 0; k < 4; k++) c |= (acc[k] / n) << (8 * k);
            out[static_cast<size_t>(y) * px + x] = c;
        }
    return out;
}

void FadeTo(winrt::Windows::UI::Composition::Compositor const& comp, winrt::Windows::UI::Composition::Visual const& v, float to) {
    auto a = comp.CreateScalarKeyFrameAnimation();
    a.InsertKeyFrame(1.0f, to, comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f}));
    a.Duration(std::chrono::milliseconds(kHoverMs));
    v.StartAnimation(L"Opacity", a);
}

}  // namespace

DockWidgets::DockWidgets(winrt::Windows::UI::Composition::Compositor const& compositor,
                         winrt::Windows::UI::Composition::CompositionGraphicsDevice const& gdev,
                         float pillHeight, float scale)
    : m_comp(compositor), m_gdev(gdev), m_scale(scale) {
    m_capH = pillHeight;  // the dock pill's own height
    m_capGap = std::round(kS3 * scale);
    m_inset = std::round(kS2 * scale);  // concentric with the pill: radius 20 less 8 is the artwork's 12
    m_art = m_capH - 2 * m_inset;       // artwork and both buttons are the same size
    m_textW = std::round(112.0f * scale);
    m_capW = std::round(m_inset + m_art + kS3 * scale + m_textW + m_inset + 2 * m_art + m_inset);
    m_thumbPx = static_cast<int>(m_art);
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                             reinterpret_cast<IUnknown**>(m_dwriteFactory.put())));
    auto make = [&](wchar_t const* family, float size, DWRITE_FONT_WEIGHT weight, winrt::com_ptr<IDWriteTextFormat>& out) {
        winrt::check_hresult(m_dwriteFactory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                               size * scale, L"en-us", out.put()));
    };
    make(IconFontFamily(m_dwriteFactory.get()), kIcon, DWRITE_FONT_WEIGHT_NORMAL, m_tfGlyph);
    make(halo::ui::kFontText, kBody, DWRITE_FONT_WEIGHT_SEMI_BOLD, m_tfTitle);
    make(halo::ui::kFontText, kCaption, DWRITE_FONT_WEIGHT_NORMAL, m_tfArtist);
    m_tfGlyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_tfGlyph->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    for (auto* f : {m_tfTitle.get(), m_tfArtist.get()}) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    m_props = compositor.CreatePropertySet();
    m_props.InsertScalar(L"Show", 0.0f);
    if (!m_gdev) return;

    // Fixed size; it fades and scales (about its left edge, so it stays beside the dock) with Show.
    m_capsule = compositor.CreateContainerVisual();
    m_capsule.Size({m_capW, m_capH});
    m_capsule.CenterPoint({0.0f, m_capH / 2, 0.0f});
    m_capsule.Opacity(0.0f);
    auto geo = compositor.CreateRoundedRectangleGeometry();
    geo.CornerRadius({halo::ui::kRadiusLarge * scale, halo::ui::kRadiusLarge * scale});
    geo.Size({m_capW, m_capH});
    m_capsule.Clip(compositor.CreateGeometricClip(geo));
    auto opacity = compositor.CreateExpressionAnimation(L"c.Show");
    opacity.SetReferenceParameter(L"c", m_props);
    m_capsule.StartAnimation(L"Opacity", opacity);
    auto scaleX = compositor.CreateExpressionAnimation(L"Vector3(0.94 + 0.06 * c.Show, 0.94 + 0.06 * c.Show, 1)");
    scaleX.SetReferenceParameter(L"c", m_props);
    m_capsule.StartAnimation(L"Scale", scaleX);

    auto highlight = [&](halo::theme::Argb c) {
        auto g = compositor.CreateEllipseGeometry();
        g.Radius({m_art / 2, m_art / 2});
        g.Center({m_art / 2, m_art / 2});
        auto shape = compositor.CreateSpriteShape(g);
        shape.FillBrush(compositor.CreateColorBrush(halo::theme::C(c)));
        auto v = compositor.CreateShapeVisual();
        v.Size({m_art, m_art});
        v.Opacity(0.0f);
        v.Shapes().Append(shape);
        return v;
    };
    m_hoverHl = highlight(halo::theme::P().hover);
    m_pressHl = highlight(halo::theme::P().press);
    m_capsule.Children().InsertAtTop(m_hoverHl);
    m_capsule.Children().InsertAtTop(m_pressHl);
    m_content = compositor.CreateSpriteVisual();
    m_content.Size({m_capW, m_capH});
    m_capsule.Children().InsertAtTop(m_content);
}

void DockWidgets::AttachCapsuleGlass(winrt::Windows::UI::Composition::ContainerVisual const& glass) {
    if (!m_capsule) return;
    glass.Size({m_capW, m_capH});
    m_capsule.Children().InsertAtBottom(glass);
}

// [0, play) is the artwork and text, then the play/pause button, then next.
float DockWidgets::PartLeft(Part p) const {
    switch (p) {
        case Part::Play: return m_capW - m_inset - 2 * m_art;
        case Part::Next: return m_capW - m_inset - m_art;
        default: return 0.0f;
    }
}

float DockWidgets::PartRight(Part p) const {
    switch (p) {
        case Part::Body: return PartLeft(Part::Play);
        case Part::Play: return PartLeft(Part::Next);
        default: return m_capW;
    }
}

void DockWidgets::AnimateShow(float to) {
    if (halo::settings::Get().reduceMotion) {
        m_props.StopAnimation(L"Show");
        m_props.InsertScalar(L"Show", to);
        return;
    }
    auto a = m_comp.CreateScalarKeyFrameAnimation();
    a.InsertKeyFrame(1.0f, to, m_comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f}));  // decelerates, no overshoot
    a.Duration(std::chrono::milliseconds(kSlowMs));
    m_props.StartAnimation(L"Show", a);
}

void DockWidgets::MoveHighlight(winrt::Windows::UI::Composition::ShapeVisual const& v, Part p, bool glide) {
    const float x = PartLeft(p);
    if (!glide) {
        v.StopAnimation(L"Offset.X");
        v.Offset({x, m_inset, 0});
        return;
    }
    v.Offset({v.Offset().x, m_inset, 0});
    auto a = m_comp.CreateScalarKeyFrameAnimation();
    a.InsertKeyFrame(1.0f, x, m_comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f}));
    a.Duration(std::chrono::milliseconds(kHoverMs));
    v.StartAnimation(L"Offset.X", a);
}

// The hover highlight glides between the two buttons and fades in and out; only the buttons have one.
void DockWidgets::SetHover(Part p) {
    if (!m_hoverHl) return;
    if (p == Part::Body || (p == Part::Next && !m_canNext) || !m_hasMedia) p = Part::None;
    if (p == m_hover) return;
    const bool was = m_hover != Part::None;
    m_hover = p;
    if (p == Part::None) {
        FadeTo(m_comp, m_hoverHl, 0.0f);
        return;
    }
    MoveHighlight(m_hoverHl, p, was);
    FadeTo(m_comp, m_hoverHl, 1.0f);
}

void DockWidgets::SetPressed(Part p) {
    if (!m_pressHl) return;
    if (p == Part::Body || (p == Part::Next && !m_canNext) || !m_hasMedia) p = Part::None;
    if (p == m_press) return;
    m_press = p;
    if (p == Part::None) {
        FadeTo(m_comp, m_pressHl, 0.0f);
        return;
    }
    MoveHighlight(m_pressHl, p, false);
    m_pressHl.StopAnimation(L"Opacity");
    m_pressHl.Opacity(1.0f);  // a press answers at once
}

void DockWidgets::SetMedia(halo::svc::MediaState const& st, bool keepPaused) {
    const bool shown = st.has && (st.playing || keepPaused);
    std::wstring title = st.title.empty() ? st.artist : st.title;
    std::wstring artist = st.title.empty() ? L"" : st.artist;
    if (st.has && title.empty()) title = L"Now Playing";
    size_t sig = 0;
    for (size_t i = 0; i < st.art.size(); i += 97) sig = sig * 31 + st.art[i];
    sig ^= static_cast<size_t>(st.artSize) << 40;
    if (m_title == title && m_artist == artist && m_hasMedia == shown && m_playing == st.playing && m_canNext == st.canNext && m_artSig == sig) return;
    const bool hadMedia = m_hasMedia;
    m_title = title;
    m_artist = artist;
    m_hasMedia = shown;
    m_playing = st.playing;
    m_canNext = st.canNext;
    if (sig != m_artSig) m_thumb = st.artSize > 0 ? Thumb(st.art, st.artSize, m_thumbPx) : std::vector<uint32_t>{};
    m_artSig = sig;
    if (!m_hasMedia || !m_gdev || !m_capsule) {
        SetHover(Part::None);
        SetPressed(Part::None);
        if (hadMedia) AnimateShow(0.0f);
        return;
    }
    if (!m_surface) {  // the surface is made when the first track appears, and kept (a few KB)
        m_surface = m_gdev.CreateDrawingSurface({m_capW, m_capH},
                                                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        m_content.Brush(m_comp.CreateSurfaceBrush(m_surface));
    }
    Render();
    if (!hadMedia) AnimateShow(1.0f);
}

// The pill's content: drawn when the track or its state changes, never while the pointer moves.
void DockWidgets::Render() {
    if (!m_surface || !m_hasMedia) return;
    auto interop = m_surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset))) return;
    try {
        const float ox = static_cast<float>(offset.x), oy = static_cast<float>(offset.y);
        const float s = m_scale;
        dc->Clear(D2D1::ColorF(0, 0.0f));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        auto const& pal = halo::theme::P();
        winrt::com_ptr<ID2D1SolidColorBrush> text, soft, faint;
        dc->CreateSolidColorBrush(pal.text, text.put());
        dc->CreateSolidColorBrush(pal.dim, soft.put());
        dc->CreateSolidColorBrush(pal.faint, faint.put());

        // Artwork: a rounded thumb (else a note on the resting fill).
        const float tx = ox + m_inset, ty = oy + m_inset, r = kRadiusSmall * s;
        const D2D1_ROUNDED_RECT thumb = D2D1::RoundedRect(D2D1::RectF(tx, ty, tx + m_art, ty + m_art), r, r);
        bool art = false;
        if (m_thumbPx > 0 && m_thumb.size() == static_cast<size_t>(m_thumbPx) * m_thumbPx) {
            winrt::com_ptr<ID2D1Bitmap> bmp;
            if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(m_thumbPx, m_thumbPx), m_thumb.data(), m_thumbPx * 4,
                                           D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)),
                                           bmp.put()))) {
                winrt::com_ptr<ID2D1BitmapBrush> brush;
                dc->CreateBitmapBrush(bmp.get(), D2D1::BitmapBrushProperties(),
                                      D2D1::BrushProperties(1.0f, D2D1::Matrix3x2F::Translation(tx, ty)), brush.put());
                dc->FillRoundedRectangle(thumb, brush.get());
                art = true;
            }
        }
        if (!art) {
            winrt::com_ptr<ID2D1SolidColorBrush> fill;
            dc->CreateSolidColorBrush(pal.fill, fill.put());
            dc->FillRoundedRectangle(thumb, fill.get());
            const wchar_t note = 0xE8D6;
            dc->DrawText(&note, 1, m_tfGlyph.get(), thumb.rect, soft.get());
        }

        // Title over artist, both ellipsised, centred as a block.
        const float textX = ox + m_inset + m_art + kS3 * s;
        auto line = [&](std::wstring const& str, IDWriteTextFormat* fmt) {
            winrt::com_ptr<IDWriteTextLayout> l;
            if (str.empty() || FAILED(m_dwriteFactory->CreateTextLayout(str.c_str(), static_cast<UINT32>(str.size()), fmt, m_textW, m_capH, l.put())))
                return winrt::com_ptr<IDWriteTextLayout>{};
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            winrt::com_ptr<IDWriteInlineObject> ellipsis;
            m_dwriteFactory->CreateEllipsisTrimmingSign(fmt, ellipsis.put());
            l->SetTrimming(&trim, ellipsis.get());
            return l;
        };
        auto t = line(m_title, m_tfTitle.get()), a = line(m_artist, m_tfArtist.get());
        DWRITE_TEXT_METRICS tm{}, am{};
        if (t) t->GetMetrics(&tm);
        if (a) a->GetMetrics(&am);
        float y = oy + (m_capH - tm.height - am.height) / 2;
        if (t) {
            dc->DrawTextLayout({textX, y}, t.get(), text.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y += tm.height;
        }
        if (a) dc->DrawTextLayout({textX, y}, a.get(), soft.get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);

        // Buttons: pause while playing, play while paused, then next (faint when the player has none).
        auto button = [&](Part p, wchar_t g, ID2D1SolidColorBrush* brush) {
            const float x = ox + PartLeft(p);
            dc->DrawText(&g, 1, m_tfGlyph.get(), D2D1::RectF(x, oy + m_inset, x + m_art, oy + m_inset + m_art), brush);
        };
        button(Part::Play, m_playing ? 0xE769 : 0xE768, text.get());
        button(Part::Next, 0xE893, m_canNext ? text.get() : faint.get());
    } catch (...) {
        interop->EndDraw();
        throw;
    }
    interop->EndDraw();
}
