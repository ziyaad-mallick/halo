#include "theme.h"
#include "cluster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <winrt/Windows.Graphics.DirectX.h>

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

    // Segoe Fluent Icons for Wi-Fi and Battery
    const float iconSize = 14.0f * m_scale;
    winrt::check_hresult(m_dwriteFactory->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
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
    m_startWidth = std::round(28.0f * s);  // one rest-size icon slot
    m_startX = m_battX + m_iconW + 3.0f * s + m_battTextW + 8.0f * s;
    m_width = std::ceil(m_startX + m_startWidth);
}

void DockCluster::Render() {
    if (!m_surface || !m_dwriteFactory) return;
    namespace conn = winrt::Windows::Networking::Connectivity;

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

        // Network: Wi-Fi or Ethernet glyph; dimmed Wi-Fi when offline.
        wchar_t net = L'\uE701';
        bool online = false;
        try {
            if (auto profile = conn::NetworkInformation::GetInternetConnectionProfile()) {
                online = profile.GetNetworkConnectivityLevel() == conn::NetworkConnectivityLevel::InternetAccess;
                if (!profile.IsWlanConnectionProfile() && !profile.IsWwanConnectionProfile()) net = L'\uE839';
            }
        } catch (...) {}
        if (!online) net = L'\uE701';
        auto netLayout = MakeLayout(std::wstring(1, net), m_tfIcons.get(), m_iconW);
        netLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        dc->DrawTextLayout({ox + m_wifiX, oy}, netLayout.get(), online ? white.get() : dim.get());

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
        const float logo = std::round(17.0f * m_scale);
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
