#pragma once
// The dock's visual tree, with magnification evaluated by the compositor.
//
// Every icon is a SpriteVisual whose Scale and Offset are expression
// animations over one property set (PointerX, Intensity) and the icons'
// own presence (P: 0 = not there, 1 = there). The app only writes PointerX on
// mouse moves and animates Intensity and presence; DWM recomputes the icons
// every frame, so a busy UI thread can't drop a dock frame. Icon brushes
// sample surfaces as large as a fully magnified icon, so scaling stays sharp.

#include <windows.h>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
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

class DockCluster;
class DockWidgets;

class Dock {
public:
    // `pill` is the background (its layers should size relative to it); the
    // dock drives its size and position and clips it to a rounded rect.
    Dock(winrt::Windows::UI::Composition::Compositor const& compositor,
         winrt::Windows::UI::Composition::ContainerVisual const& parent,
         winrt::Windows::UI::Composition::ContainerVisual const& pill,
         DockMetrics const& metrics,
         DockCluster* cluster = nullptr,
         DockWidgets* widgets = nullptr);  // HALO 2.0: Wi-Fi + volume on the right of the pill

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

    // Removes the dock's visuals from the parent (to replace it with a rebuilt one).
    void Detach();

    // Pointer in the parent's coordinates. Move starts magnifying if needed.
    void OnPointerMove(float x);
    void OnPointerLeave();

    // What the point is on, as currently drawn. Hit-testing is magnetic: the point snaps to the nearest target
    // (an app icon, the Start logo or, with HALO 2.0, the whole left cluster, the Wi-Fi and volume widgets), so
    // the gaps between targets and the pill's edges are not dead, and a little above the pill still counts.
    // Media is the Now Playing capsule's artwork and title; MediaPlay and MediaNext are its buttons.
    enum class Kind { None, App, Start, Media, MediaPlay, MediaNext };
    struct Target {
        Kind kind = Kind::None;
        int index = -1;  // App: into the last SetItems
    };
    Target HitTarget(float x, float y) const;

    // Index (into the last SetItems) of the icon under the point as currently drawn, or -1.
    int HitTest(float x, float y) const;

    // The icon eases a few px toward the pointer while it is the one hovered (index into the last SetItems; -1
    // lets go). Off under reduce motion.
    void SetMagnet(int index);

    // Darkens icon `index` while the button is down on it; -1 lets go.
    void SetPressed(int index);

    // Bounces icon `index` three times, or until it is running.
    void Bounce(int index);

    // One short bob (half a bounce high) on icon `index`: feedback for clicking an app that is running.
    void Bob(int index);

    // Where icon `index` (into the last SetItems) is centered with the dock at
    // rest (no magnification), in the parent's coordinates.
    std::pair<float, float> RestIconCenter(size_t index) const;

    // Where icon `index` is centered and top edge under current magnification,
    // in parent visual coordinates. Returns {centerX, topY}.
    std::pair<float, float> MagnifiedIconCenterAndTop(size_t index) const;

    // Whether (x, y) is on the cluster's Start button (HALO 2.0: anywhere in the cluster).
    bool HitStart(float x, float y) const;

    // Cluster event forwarding
    void OnClockTimer();
    void OnTimeChange();
    void OnSettingChange();
    void OnPowerBroadcast(WPARAM w, LPARAM l);
    void OnNetworkChanged();

    // Width of the pill at rest and at full magnification (with every icon
    // still on its way out counted), for sizing the window's input region.
    // How far the Now Playing capsule reaches right of the pill (its gap and width once open; 0 while hidden), for the
    // window's input region.
    float CapsuleExtent() const;
    float RestWidth() const;
    float MaxWidth() const;

    // How many icons fit in `availWidth` px with the wave at full magnification (at least 1).
    size_t MaxItems(float availWidth) const;

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
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);  // batch callbacks hold it weakly: they outlive a rebuilt dock
    DockMetrics m_m;
    bool m_hovering = false;
    float m_pointerX = 0.0f;

    DockCluster* m_cluster = nullptr;
    DockWidgets* m_widgets = nullptr;
    bool m_h2 = false;           // HALO 2.0 at construction (the dock is rebuilt when it flips)
    float m_shift = 0.0f;        // (cluster - widgets) / 2: where the app icons' centre sits relative to the dock's
    unsigned m_magnetId = 0;     // the entry id pulled toward the pointer
    winrt::Windows::UI::Composition::SpriteVisual m_clusterDivider{nullptr};
    float m_clusterWidth = 0.0f;
    float m_dividerWidth = 0.0f;
    float m_clusterOffset = 0.0f;
};
