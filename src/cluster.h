#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include "services.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace halo::hit {

// Magnetic hit-testing on one axis: spans are the clickable ranges left to right (they may touch or leave
// gaps); a point snaps to the nearest one, so there are no dead gaps between targets or at the pill's edges.
struct Span {
    float left, right;
};

// Index of the span nearest to x (0 distance = inside; a tie goes to the span whose centre is nearer), or -1
// when the nearest one is farther than `tol`.
inline int Nearest(std::vector<Span> const& spans, float x, float tol) {
    int best = -1;
    float bestD = 0.0f, bestC = 0.0f;
    for (size_t i = 0; i < spans.size(); i++) {
        const float d = x < spans[i].left ? spans[i].left - x : x > spans[i].right ? x - spans[i].right : 0.0f;
        const float c = std::abs(x - (spans[i].left + spans[i].right) / 2);
        if (best < 0 || d < bestD || (d == bestD && c < bestC)) {
            best = static_cast<int>(i);
            bestD = d;
            bestC = c;
        }
    }
    return best >= 0 && bestD <= tol ? best : -1;
}

// Vertical tolerance: the point may sit `tol` above `top` (the target's highest edge); anything below it is in.
inline bool BelowTop(float y, float top, float tol) { return y >= top - tol; }

}  // namespace halo::hit

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
    // Anywhere in the cluster (the time, Wi-Fi, battery, Start and the gaps between): HALO 2.0 opens Start from all of it.
    bool HitTestCluster(float localX, float localY) const;


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

// HALO 2.0 Now Playing pill: a separate floating pill beside the dock (same height, glass, radius and hairline as
// the dock's), [artwork][title over artist][play/pause][next]. Drawn only when the track or its state changes;
// the buttons' hover and pressed highlights are small visuals whose opacity the compositor animates, and the pill
// fades and scales in and out with the "Show" property of Properties() (0 = nothing playing: transparent, no cost).
class DockWidgets {
public:
    enum class Part { None, Body, Play, Next };

    DockWidgets(winrt::Windows::UI::Composition::Compositor const& compositor,
                winrt::Windows::UI::Composition::CompositionGraphicsDevice const& gdev,
                float pillHeight,
                float scale);

    winrt::Windows::UI::Composition::ContainerVisual Capsule() const { return m_capsule; }
    winrt::Windows::UI::Composition::CompositionPropertySet Properties() const { return m_props; }
    float CapsuleHeight() const { return m_capH; }
    float CapsuleWidth() const { return m_capW; }
    float CapsuleGap() const { return m_capGap; }
    bool CapsuleShown() const { return m_hasMedia; }
    // Horizontal extent of a part, in the pill's coordinates.
    float PartLeft(Part p) const;
    float PartRight(Part p) const;
    bool CanNext() const { return m_canNext; }
    // The glass (built by the owner, so it is the dock's) goes underneath everything else.
    void AttachCapsuleGlass(winrt::Windows::UI::Composition::ContainerVisual const& glass);
    void SetHover(Part p);
    void SetPressed(Part p);
    void SetMedia(halo::svc::MediaState const& state);

private:
    void Render();
    void AnimateShow(float to);
    void MoveHighlight(winrt::Windows::UI::Composition::ShapeVisual const& v, Part p, bool glide);

    winrt::Windows::UI::Composition::Compositor m_comp{nullptr};
    winrt::Windows::UI::Composition::CompositionGraphicsDevice m_gdev{nullptr};
    winrt::Windows::UI::Composition::CompositionDrawingSurface m_surface{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_content{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_capsule{nullptr};
    winrt::Windows::UI::Composition::ShapeVisual m_hoverHl{nullptr}, m_pressHl{nullptr};
    winrt::Windows::UI::Composition::CompositionPropertySet m_props{nullptr};
    winrt::com_ptr<IDWriteFactory> m_dwriteFactory;
    winrt::com_ptr<IDWriteTextFormat> m_tfGlyph, m_tfTitle, m_tfArtist;
    float m_scale = 1.0f;
    bool m_hasMedia = false, m_playing = false, m_canNext = false;
    std::wstring m_title, m_artist;
    float m_capH = 50.0f, m_capW = 0.0f, m_capGap = 12.0f, m_inset = 8.0f, m_art = 34.0f, m_textW = 0.0f;
    Part m_hover = Part::None, m_press = Part::None;
    int m_thumbPx = 0;
    std::vector<uint32_t> m_thumb;  // the artwork box-filtered to the thumb size (a few KB, not the source image)
    size_t m_artSig = 0;
};
