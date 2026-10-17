#pragma once
// The dock's visual tree, with magnification evaluated by the compositor.
//
// Every icon is a SpriteVisual whose Scale and Offset are expression
// animations over one property set (PointerX, Intensity) and the icons'
// own presence (P: 0 = not there, 1 = there). The app only writes PointerX on
// mouse moves and animates Intensity and presence; DWM recomputes the icons
// every frame, so a busy UI thread can't drop a dock frame. Icon brushes
// sample surfaces as large as a fully magnified icon, so scaling stays sharp.

#include <chrono>
#include <string>
#include <vector>

#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>

struct DockMetrics {
    float restSize = 28.0f;    // icon size at rest, px
    float gap = 10.0f;         // space between icons at rest, px
    float maxScale = 1.57f;    // magnification under the cursor
    float radius = 100.0f;     // how far the wave reaches, px
    float padding = 10.0f;     // pill padding around the outer icons, px
    float pillHeight = 50.0f;  // background height at rest, px
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
        std::wstring key;  // identity across calls
        winrt::Windows::UI::Composition::CompositionBrush brush;
        bool running;  // gets a dot underneath
        bool extra;    // not pinned: after a divider, below the pinned apps
    };
    // The icons, left to right. Icons whose key is new grow in, ones that are
    // gone shrink out, the rest slide to their new place. The first call
    // shows everything at once.
    void SetItems(std::vector<Item> const& items);

    // Pointer in the parent's coordinates. Move starts magnifying if needed.
    void OnPointerMove(float x);
    void OnPointerLeave();

    // Index (into the last SetItems) of the icon under the point as currently drawn, or -1.
    int HitTest(float x, float y) const;

    // Darkens icon `index` while the button is down on it; -1 lets go.
    void SetPressed(int index);

    // Bounces icon `index` three times, or until it is running.
    void Bounce(int index);

    // Width of the pill at rest and at full magnification (with every icon
    // still on its way out counted), for sizing the window's input region.
    float RestWidth() const;
    float MaxWidth() const;

private:
    struct Entry {
        std::wstring key;
        winrt::Windows::UI::Composition::CompositionBrush brush{nullptr};
        winrt::Windows::UI::Composition::SpriteVisual icon{nullptr};
        winrt::Windows::UI::Composition::ShapeVisual dot{nullptr};
        unsigned id = 0;
        bool running = false, extra = false, leaving = false;
        std::chrono::steady_clock::time_point bounceStart{};
    };
    float DotSize() const;
    float IconY() const;  // Offset.Y of an icon at rest
    Entry MakeEntry(Item const& item, bool instant);
    void AnimateTo(winrt::Windows::UI::Composition::CompositionObject const& target, wchar_t const* property,
                   float to, int ms);
    void Prune(std::vector<unsigned> const& ids);
    void BuildExpressions();
    bool Bouncing(Entry const& e) const;
    void Settle(Entry& e);

    winrt::Windows::UI::Composition::Compositor m_compositor;
    winrt::Windows::UI::Composition::ContainerVisual m_root{nullptr};
    winrt::Windows::UI::Composition::ContainerVisual m_pill{nullptr};
    winrt::Windows::UI::Composition::CompositionPropertySet m_props{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_divider{nullptr};  // its property "Slot" is the width it takes
    winrt::Windows::UI::Composition::CubicBezierEasingFunction m_easeOut{nullptr};
    winrt::Windows::UI::Composition::SpriteVisual m_overlay{nullptr};  // darkens the pressed icon
    std::vector<Entry> m_entries;  // includes the ones shrinking out
    int m_dividerAt = -1;          // index in m_entries of the first unpinned icon, or -1
    bool m_dividerShown = false;
    unsigned m_nextId = 1;
    DockMetrics m_m;
    bool m_hovering = false;
    float m_pointerX = 0.0f;
};
