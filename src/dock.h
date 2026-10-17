#pragma once
// The dock's visual tree, with magnification evaluated by the compositor.
//
// Every icon is a SpriteVisual whose Scale and Offset are expression
// animations over one property set (PointerX, Intensity). The app only
// writes PointerX on mouse moves and animates Intensity on enter/leave;
// DWM recomputes the icons every frame, so a busy UI thread can't drop a
// dock frame. Icon brushes sample 256px surfaces, so scaling stays sharp.

#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>

struct DockMetrics {
    float restSize = 44.0f;    // icon size at rest, px
    float gap = 10.0f;         // space between icons at rest, px
    float maxScale = 1.6f;     // magnification under the cursor
    float radius = 130.0f;     // how far the wave reaches, px
    float padding = 12.0f;     // pill padding around the outer icons, px
    float pillHeight = 64.0f;  // background height at rest, px
    float cornerRadius = 20.0f;
    float bottom = 0.0f;       // y of the pill's bottom edge in the parent
    float centerX = 0.0f;      // x of the dock's center in the parent
};

class Dock {
public:
    // `pill` is the background (its layers should size relative to it); the
    // dock drives its size and position and clips it to a rounded rect.
    Dock(winrt::Windows::UI::Composition::Compositor const& compositor,
         winrt::Windows::UI::Composition::ContainerVisual const& parent,
         winrt::Windows::UI::Composition::ContainerVisual const& pill,
         DockMetrics const& metrics);

    struct Item {
        winrt::Windows::UI::Composition::CompositionBrush brush;
        bool running;  // gets a dot underneath
    };
    // One item per icon, left to right; a thin divider is drawn before item
    // `divider` (none unless it is in 1..count-1). Rebuilds the icon visuals.
    void SetIcons(std::vector<Item> const& items, size_t divider);

    // Moves the dock's center (the window was resized).
    void SetCenter(float x);

    // Pointer in the parent's coordinates. Move starts magnifying if needed.
    void OnPointerMove(float x);
    void OnPointerLeave();

    // Index of the icon under x (parent coordinates) as currently drawn, or -1.
    int HitTest(float x, float y) const;

    // Where icon i is centered with the dock at rest (no magnification), in the parent's coordinates.
    std::pair<float, float> RestIconCenter(size_t i) const;

    // Width of the pill at rest and at full magnification, for sizing the window.
    float RestWidth() const;
    float MaxWidth() const;

private:
    float DotSize() const;
    float RestCenter(size_t i) const;
    float MagnificationAt(float pointerX, size_t i) const;  // CPU mirror of the expression
    void AnimateIntensity(float to);
    void BuildExpressions();

    winrt::Windows::UI::Composition::Compositor m_compositor;
    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_pill{nullptr};
    winrt::Windows::UI::Composition::CompositionPropertySet m_props{nullptr};
    std::vector<winrt::Windows::UI::Composition::SpriteVisual> m_icons;
    std::vector<winrt::Windows::UI::Composition::Visual> m_dots;  // parallel to m_icons; null if not running
    winrt::Windows::UI::Composition::SpriteVisual m_divider{nullptr};
    size_t m_dividerAt = 0;
    DockMetrics m_m;
    bool m_hovering = false;
    float m_pointerX = 0.0f;
};
