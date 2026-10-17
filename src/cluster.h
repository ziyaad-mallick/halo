#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <string>

// Status cluster on the left side of the Halo dock pill:
// - Time: Segoe UI Variable Text (13pt tabular figures)
// - Network: Segoe Fluent Icons Wi-Fi (\uE701, dimmed offline) or Ethernet (\uE839)
// - Battery: Segoe Fluent Icons Battery0..10 / BatteryCharging0..10 + percentage
// - Start button: the Windows 11 logo, drawn
// Event-driven, 0 CPU idle architecture.
class DockCluster {
public:
    DockCluster(winrt::Windows::UI::Composition::Compositor const& compositor,
                winrt::Windows::UI::Composition::CompositionGraphicsDevice const& gdev,
                HWND hwnd,
                float pillHeight,
                float scale);
    ~DockCluster();

    winrt::Windows::UI::Composition::SpriteVisual Visual() const { return m_visual; }

    float Width() const { return m_width; }
    float StartX() const { return m_startX; }
    float StartWidth() const { return m_startWidth; }

    // Hit-testing within cluster coordinates [0..Width, 0..PillHeight].
    bool HitTestStart(float localX, float localY) const;


    // Event handlers
    void OnTimer(UINT_PTR timerId);
    void OnTimeChange();
    void OnSettingChange();
    void OnPowerBroadcast(WPARAM w, LPARAM l);
    void OnNetworkChanged();

    // Redraw the cluster into the drawing surface
    void Render();

    static constexpr UINT_PTR kClockTimer = 2;
    static constexpr UINT kMsgNetworkChanged = WM_APP + 4;

private:
    void InitDirectWrite();
    void Layout();
    winrt::com_ptr<IDWriteTextLayout> MakeLayout(std::wstring const& text, IDWriteTextFormat* format, float width) const;
    float TextWidth(std::wstring const& text, IDWriteTextFormat* format) const;
    void SetupClockTimer();
    void RegisterPowerNotifications();
    void UnregisterPowerNotifications();
    void RegisterNetworkCallback();
    void UnregisterNetworkCallback();

    winrt::Windows::UI::Composition::Compositor m_compositor{nullptr};
    winrt::Windows::UI::Composition::CompositionGraphicsDevice m_gdev{nullptr};
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_surface{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_visual{nullptr};

    HWND m_hwnd{};
    float m_pillHeight = 50.0f;
    float m_scale = 1.0f;
    float m_width = 0.0f;
    float m_startX = 0.0f;
    float m_startWidth = 0.0f;
    float m_timeX = 0.0f, m_timeW = 0.0f, m_wifiX = 0.0f, m_iconW = 0.0f, m_battX = 0.0f, m_battTextW = 0.0f;

    // DirectWrite objects
    winrt::com_ptr<IDWriteFactory> m_dwriteFactory;
    winrt::com_ptr<IDWriteTextFormat> m_tfTime;
    winrt::com_ptr<IDWriteTextFormat> m_tfIcons;
    winrt::com_ptr<IDWriteTextFormat> m_tfBattText;
    winrt::com_ptr<IDWriteTypography> m_typography;

    // Power notifications
    HPOWERNOTIFY m_powerNotifyBatt = nullptr;
    HPOWERNOTIFY m_powerNotifyAcDc = nullptr;

    // Network callback
    winrt::event_token m_networkToken{};
    bool m_networkRegistered = false;
};
