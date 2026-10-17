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

constexpr ULONG_PTR kHaloInputMarker = 0x48414C4F;  // "HALO"

}  // namespace

DockCluster::DockCluster(winrt::Windows::UI::Composition::Compositor const& compositor,
                         winrt::Windows::UI::Composition::CompositionGraphicsDevice const& gdev,
                         HWND hwnd,
                         float pillHeight,
                         float scale)
    : m_compositor(compositor), m_gdev(gdev), m_hwnd(hwnd), m_pillHeight(pillHeight), m_scale(scale) {
    m_width = std::round(192.0f * m_scale);
    m_startWidth = std::round(28.0f * m_scale);
    m_startX = m_width - m_startWidth;

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

    InitDirectWrite();
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

    // Segoe Fluent Icons for Windows 11 Start logo
    const float startSize = 16.0f * m_scale;
    winrt::check_hresult(m_dwriteFactory->CreateTextFormat(
        L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, startSize,
        L"en-us", m_tfStart.put()));
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

void DockCluster::TriggerStartMenu() {
    INPUT in[4] = {};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_CONTROL;
    in[0].ki.dwExtraInfo = kHaloInputMarker;

    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = VK_ESCAPE;
    in[1].ki.dwExtraInfo = kHaloInputMarker;

    in[2].type = INPUT_KEYBOARD;
    in[2].ki.wVk = VK_ESCAPE;
    in[2].ki.dwFlags = KEYEVENTF_KEYUP;
    in[2].ki.dwExtraInfo = kHaloInputMarker;

    in[3].type = INPUT_KEYBOARD;
    in[3].ki.wVk = VK_CONTROL;
    in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    in[3].ki.dwExtraInfo = kHaloInputMarker;

    SendInput(4, in, sizeof(INPUT));
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

void DockCluster::Render() {
    if (!m_surface || !m_dwriteFactory) return;

    auto interop = m_surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    HRESULT hr = interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset);
    if (FAILED(hr)) return;

    try {
        dc->Clear(D2D1::ColorF(0, 0.0f));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        winrt::com_ptr<ID2D1SolidColorBrush> whiteBrush;
        winrt::com_ptr<ID2D1SolidColorBrush> dimBrush;
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), whiteBrush.put());
        dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.40f), dimBrush.put());

        // 1. Time string formatted according to user settings ("9:41" / "17:40")
        SYSTEMTIME st;
        GetLocalTime(&st);
        int is24 = 0;
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_ITIME | LOCALE_RETURN_NUMBER,
                        reinterpret_cast<LPWSTR>(&is24), sizeof(is24) / sizeof(wchar_t));
        int hour = is24 ? st.wHour : (st.wHour % 12 == 0 ? 12 : st.wHour % 12);
        wchar_t timeBuf[32];
        swprintf(timeBuf, 32, is24 ? L"%02d:%02d" : L"%d:%02d", hour, st.wMinute);

        winrt::com_ptr<IDWriteTextLayout> timeLayout;
        m_dwriteFactory->CreateTextLayout(
            timeBuf, static_cast<UINT32>(wcslen(timeBuf)),
            m_tfTime.get(), 1000.0f, m_pillHeight, timeLayout.put());
        timeLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (m_typography) {
            DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(wcslen(timeBuf))};
            timeLayout->SetTypography(m_typography.get(), range);
        }
        float timeX = 6.0f * m_scale;
        dc->DrawTextLayout(D2D1::Point2F(offset.x + timeX, offset.y + 0), timeLayout.get(), whiteBrush.get());

        // 2. Wi-Fi glyph (\uE701 connected, \uEB55 disconnected)
        bool wifiConnected = false;
        try {
            auto profile = winrt::Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
            if (profile) {
                wifiConnected = (profile.GetNetworkConnectivityLevel() ==
                                 winrt::Windows::Networking::Connectivity::NetworkConnectivityLevel::InternetAccess);
            }
        } catch (...) {}
        wchar_t wifiGlyph[2] = { wifiConnected ? L'\uE701' : L'\uEB55', 0 };
        winrt::com_ptr<IDWriteTextLayout> wifiLayout;
        m_dwriteFactory->CreateTextLayout(
            wifiGlyph, 1, m_tfIcons.get(), 1000.0f, m_pillHeight, wifiLayout.put());
        wifiLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        float wifiX = 60.0f * m_scale;
        dc->DrawTextLayout(D2D1::Point2F(offset.x + wifiX, offset.y + 0), wifiLayout.get(),
                           wifiConnected ? whiteBrush.get() : dimBrush.get());

        // 3. Battery status
        SYSTEM_POWER_STATUS sps{};
        bool hasBattery = false;
        int batteryPercent = 100;
        bool acPlugged = false;
        if (GetSystemPowerStatus(&sps)) {
            hasBattery = !(sps.BatteryFlag & 128) && sps.BatteryLifePercent <= 100;
            batteryPercent = sps.BatteryLifePercent;
            acPlugged = (sps.ACLineStatus == 1);
        }

        float battX = 82.0f * m_scale;
        if (hasBattery) {
            int level = std::clamp((batteryPercent + 5) / 10, 0, 10);
            wchar_t battGlyph[2] = { static_cast<wchar_t>(0xEBA0 + level), 0 };
            winrt::com_ptr<IDWriteTextLayout> battLayout;
            m_dwriteFactory->CreateTextLayout(
                battGlyph, 1, m_tfIcons.get(), 1000.0f, m_pillHeight, battLayout.put());
            battLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            DWRITE_TEXT_METRICS bm{};
            battLayout->GetMetrics(&bm);
            dc->DrawTextLayout(D2D1::Point2F(offset.x + battX, offset.y + 0), battLayout.get(), whiteBrush.get());

            battX += bm.widthIncludingTrailingWhitespace + 4.0f * m_scale;

            wchar_t battTextBuf[16];
            swprintf(battTextBuf, 16, L"%d%%", batteryPercent);
            winrt::com_ptr<IDWriteTextLayout> battTextLayout;
            m_dwriteFactory->CreateTextLayout(
                battTextBuf, static_cast<UINT32>(wcslen(battTextBuf)),
                m_tfBattText.get(), 1000.0f, m_pillHeight, battTextLayout.put());
            battTextLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            if (m_typography) {
                DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(wcslen(battTextBuf))};
                battTextLayout->SetTypography(m_typography.get(), range);
            }
            DWRITE_TEXT_METRICS btm{};
            battTextLayout->GetMetrics(&btm);
            dc->DrawTextLayout(D2D1::Point2F(offset.x + battX, offset.y + 0), battTextLayout.get(), whiteBrush.get());

            battX += btm.widthIncludingTrailingWhitespace;

            if (acPlugged) {
                battX += 3.0f * m_scale;
                wchar_t boltGlyph[2] = { L'\uEBB5', 0 };
                winrt::com_ptr<IDWriteTextLayout> boltLayout;
                m_dwriteFactory->CreateTextLayout(
                    boltGlyph, 1, m_tfIcons.get(), 1000.0f, m_pillHeight, boltLayout.put());
                boltLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                dc->DrawTextLayout(D2D1::Point2F(offset.x + battX, offset.y + 0), boltLayout.get(), whiteBrush.get());
            }
        } else if (acPlugged) {
            wchar_t boltGlyph[2] = { L'\uEBB5', 0 };
            winrt::com_ptr<IDWriteTextLayout> boltLayout;
            m_dwriteFactory->CreateTextLayout(
                boltGlyph, 1, m_tfIcons.get(), 1000.0f, m_pillHeight, boltLayout.put());
            boltLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            dc->DrawTextLayout(D2D1::Point2F(offset.x + battX, offset.y + 0), boltLayout.get(), whiteBrush.get());
        }

        // 4. Start Button (\uE782 in Segoe Fluent Icons, centered in Start slot)
        wchar_t startGlyph[2] = { L'\uE782', 0 };
        winrt::com_ptr<IDWriteTextLayout> startLayout;
        m_dwriteFactory->CreateTextLayout(
            startGlyph, 1, m_tfStart.get(), m_startWidth, m_pillHeight, startLayout.put());
        startLayout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        startLayout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawTextLayout(D2D1::Point2F(offset.x + m_startX, offset.y + 0), startLayout.get(), whiteBrush.get());
    } catch (...) {
        interop->EndDraw();
        throw;
    }
    interop->EndDraw();
}
