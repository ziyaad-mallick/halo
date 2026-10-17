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
// - Wi-Fi: Segoe Fluent Icons glyph (\uE701 connected, \uEB55 disconnected)
// - Battery: Segoe Fluent Icons glyph (\uEBA0..\uEBAA) + percentage + bolt (\uEBB5)
// - Start button: Windows 11 logo glyph (\uE782)
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

    // Injects Ctrl+Esc down/up with dwExtraInfo = 0x48414C4F to open Start menu.
    static void TriggerStartMenu();

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

    // DirectWrite objects
    winrt::com_ptr<IDWriteFactory> m_dwriteFactory;
    winrt::com_ptr<IDWriteTextFormat> m_tfTime;
    winrt::com_ptr<IDWriteTextFormat> m_tfIcons;
    winrt::com_ptr<IDWriteTextFormat> m_tfBattText;
    winrt::com_ptr<IDWriteTextFormat> m_tfStart;
    winrt::com_ptr<IDWriteTypography> m_typography;

    // Power notifications
    HPOWERNOTIFY m_powerNotifyBatt = nullptr;
    HPOWERNOTIFY m_powerNotifyAcDc = nullptr;

    // Network callback
    winrt::event_token m_networkToken{};
    bool m_networkRegistered = false;
};
