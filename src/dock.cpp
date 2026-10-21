#include "theme.h"
#include "dock.h"
#include "cluster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>

using namespace winrt;
using namespace winrt::Windows::Foundation::Numerics;
using namespace winrt::Windows::UI::Composition;

namespace {

constexpr float kPi = 3.14159265f;
constexpr int kFocusMs = 90;      // magnification in and out
constexpr int kMagnetMs = 70;     // the pull toward the pointer easing in and out
constexpr float kPullPx = 2.0f;    // its reach at the rest size of 28
constexpr int kPresenceMs = 250;   // an icon growing in or shrinking out
constexpr int kFadeMs = 150;       // running dots
constexpr int kPressInMs = 60;
constexpr int kPressOutMs = 100;
constexpr float kPressDarkness = 0.3f;
constexpr int kBounceMs = 350;     // one bounce, up and back down
constexpr int kBounces = 3;
constexpr float kDotOpacity = 0.7f;
// [taskbar] running_dots off: the dots are never shown.
float DotOpacity() { return halo::settings::Get().taskbar.runningDots ? kDotOpacity : 0.0f; }

// Starts an animation, naming the property and expression if it is rejected.
void Start(CompositionObject const& target, wchar_t const* property, CompositionAnimation const& anim,
           std::wstring const& what) {
    try {
        target.StartAnimation(property, anim);
    } catch (hresult_error const& e) {
        std::wstring msg = L"[halo] StartAnimation(" + std::wstring(property) + L") failed for " + what + L": " +
                           std::wstring(e.message()) + L"\n";
        OutputDebugStringW(msg.c_str());
        fputws(msg.c_str(), stderr);
        throw;
    }
}

// A bounce eases out on the way up and in on the way down.
CubicBezierEasingFunction BounceUp(Compositor const& c) { return c.CreateCubicBezierEasingFunction({0.3f, 0.5f}, {0.5f, 1.0f}); }
CubicBezierEasingFunction BounceDown(Compositor const& c) { return c.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.7f, 0.5f}); }

// Expression parameter name of icon j's property set.
std::wstring Param(size_t j) { return L"s" + std::to_wstring(j); }

// Width of the pill for `count` icons at rest.
float WidthAtRest(DockMetrics const& m, size_t count, bool divider) {
    return static_cast<float>(count) * (m.restSize + m.gap) - m.gap + 2 * m.padding + (divider ? m.gap : 0.0f);
}

// What the wave adds to the pill at full magnification, in icon pitches (the icon under the cursor counts one more).
float WaveGrowth(DockMetrics const& m) {
    float pitch = m.restSize + m.gap;
    float growth = 0.0f;
    for (float d = -m.radius; d <= m.radius; d += pitch) {
        growth += (m.maxScale - 1) * (std::cos(std::abs(d) / m.radius * kPi) + 1) / 2;
    }
    return growth + 1;
}

}  // namespace

Dock::Dock(Compositor const& compositor, ContainerVisual const& parent,
           ContainerVisual const& pill, DockMetrics const& metrics,
           DockCluster* cluster, DockWidgets* widgets)
    : m_compositor(compositor), m_pill(pill), m_m(metrics), m_cluster(cluster), m_widgets(widgets),
      m_h2(halo::settings::Get().halo2) {
    m_root = m_compositor.CreateContainerVisual();
    m_root.RelativeSizeAdjustment({1.0f, 1.0f});
    parent.Children().InsertAtTop(m_root);
    m_easeOut = m_compositor.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f});

    m_props = m_compositor.CreatePropertySet();
    m_props.InsertScalar(L"PointerX", 0.0f);
    m_props.InsertScalar(L"Intensity", 0.0f);
    m_props.InsertScalar(L"MaxScale", m_m.maxScale);
    m_props.InsertScalar(L"Radius", m_m.radius);
    m_props.InsertScalar(L"Pitch", m_m.restSize + m_m.gap);
    m_props.InsertScalar(L"Rest", m_m.restSize);
    m_props.InsertScalar(L"Gap", m_m.gap);
    m_props.InsertScalar(L"Padding", m_m.padding);
    m_props.InsertScalar(L"Center", m_m.centerX);
    m_props.InsertScalar(L"ContentCenter", m_m.centerX);
    if (m_widgets) {
        auto center = m_compositor.CreateExpressionAnimation(L"p.Center - mc.Show * " +
            std::to_wstring((m_widgets->CapsuleGap() + m_widgets->CapsuleWidth()) / 2));
        center.SetReferenceParameter(L"p", m_props);
        center.SetReferenceParameter(L"mc", m_widgets->Properties());
        Start(m_props, L"ContentCenter", center, L"dock and media center");
    }

    if (m_cluster) {
        m_clusterWidth = m_cluster->Width();
        m_dividerWidth = m_m.gap;
        m_clusterOffset = m_clusterWidth + m_dividerWidth;

        // A 1px subtle divider between the left cluster and the app icons
        const float cw = std::max(1.0f, std::round(m_m.restSize / 28));
        const float ch = m_m.pillHeight * 0.6f;
        m_clusterDivider = m_compositor.CreateSpriteVisual();
        m_clusterDivider.Size({cw, ch});
        m_clusterDivider.Brush(m_compositor.CreateColorBrush(halo::theme::C(halo::theme::P().divider)));
        m_clusterDivider.Offset({0, m_m.bottom - (m_m.pillHeight + ch) / 2, 0});
        m_root.Children().InsertAtTop(m_clusterDivider);
        if (m_cluster->Visual()) {
            m_cluster->Visual().Offset({0, m_m.bottom - m_m.pillHeight, 0});
            m_root.Children().InsertAtTop(m_cluster->Visual());
        }
    }

    if (m_widgets && m_widgets->Capsule()) {  // the Now Playing pill floats beside the dock, level with it
        m_widgets->Capsule().Offset({0, m_m.bottom - m_m.pillHeight, 0});
        m_root.Children().InsertAtTop(m_widgets->Capsule());
    }
    m_shift = m_clusterOffset / 2;

    // A 1px line, 60% of the pill high, in a slot one gap wide: a full gap
    // each side. Its slot and opacity animate when the first unpinned icon
    // arrives and the last one leaves.
    const float w = std::max(1.0f, std::round(m_m.restSize / 28));
    const float h = m_m.pillHeight * 0.6f;
    m_divider = m_compositor.CreateSpriteVisual();
    m_divider.Size({w, h});
    m_divider.Brush(m_compositor.CreateColorBrush(halo::theme::C(halo::theme::P().divider)));
    m_divider.Offset({0, m_m.bottom - (m_m.pillHeight + h) / 2, 0});
    m_divider.Opacity(0.0f);
    m_divider.Properties().InsertScalar(L"Slot", 0.0f);
    m_root.Children().InsertAtTop(m_divider);

    m_pill.Size({m_clusterOffset + WidthAtRest(m_m, 0, false), m_m.pillHeight});
    m_pill.Offset({m_m.centerX - m_pill.Size().x / 2, m_m.bottom - m_m.pillHeight, 0});
    auto clipShape = m_compositor.CreateRoundedRectangleGeometry();
    clipShape.CornerRadius({m_m.cornerRadius, m_m.cornerRadius});
    clipShape.Size(m_pill.Size());
    m_pill.Clip(m_compositor.CreateGeometricClip(clipShape));
    m_root.Children().InsertAtBottom(m_pill);
}

float Dock::CapsuleExtent() const {
    return m_widgets && m_widgets->CapsuleShown() ? m_widgets->CapsuleGap() + m_widgets->CapsuleWidth() : 0.0f;
}

float Dock::DotSize() const { return m_m.restSize / 7; }  // 4 logical px at the rest size of 28

float Dock::IconY() const { return m_m.bottom - (m_m.pillHeight - m_m.restSize) / 2 - m_m.restSize; }

float Dock::RestWidth() const {
    size_t n = 0;
    bool extra = false;
    for (auto const& e : m_entries) {
        if (e.leaving) continue;
        n++;
        extra |= e.extra && n > 1;
    }
    return m_clusterOffset + WidthAtRest(m_m, n, extra);
}

float Dock::MaxWidth() const {
    // The wave adds at most about 2 * radius / pitch icons' worth of growth.
    return m_clusterOffset + WidthAtRest(m_m, m_entries.size(), true) + WaveGrowth(m_m) * (m_m.restSize + m_m.gap);
}

size_t Dock::MaxItems(float availWidth) const {
    // MaxWidth solved for the icon count: any more and the magnified pill would run off the screen.
    float pitch = m_m.restSize + m_m.gap;
    float room = availWidth - m_clusterOffset - WidthAtRest(m_m, 0, true) - WaveGrowth(m_m) * pitch;
    return room < pitch ? 1 : static_cast<size_t>(room / pitch);
}

void Dock::AnimateTo(CompositionObject const& target, wchar_t const* property, float to, int ms) {
    auto a = m_compositor.CreateScalarKeyFrameAnimation();
    a.InsertKeyFrame(1.0f, to, m_easeOut);
    a.Duration(std::chrono::milliseconds(ms));
    Start(target, property, a, property);
}

Dock::Entry Dock::MakeEntry(Item const& item, bool instant) {
    Entry e;
    e.key = item.key;
    e.brush = item.brush;
    e.id = m_nextId++;
    e.running = item.running;
    e.extra = item.extra;

    auto v = m_compositor.CreateSpriteVisual();
    v.Brush(item.brush);
    v.Size({m_m.restSize, m_m.restSize});
    // Grow upward from the bottom-center, like the macOS dock.
    v.CenterPoint({m_m.restSize / 2, m_m.restSize, 0});
    v.Offset({0, IconY(), 0});
    v.Properties().InsertScalar(L"P", instant ? 1.0f : 0.0f);  // presence
    v.Properties().InsertScalar(L"S", instant ? 1.0f : 0.0f);  // scale, set by an expression
    v.Properties().InsertScalar(L"Mag", 0.0f);                 // 0..1: how far the magnetic pull toward the pointer has eased in
    m_root.Children().InsertAtTop(v);
    e.icon = v;

    // The running dot sits one dot-size under the icon and stays put.
    const float dot = DotSize();
    auto geometry = m_compositor.CreateEllipseGeometry();
    geometry.Radius({dot / 2, dot / 2});
    auto shape = m_compositor.CreateSpriteShape(geometry);
    shape.Offset({dot / 2, dot / 2});
    shape.FillBrush(m_compositor.CreateColorBrush(halo::theme::C(halo::theme::P().dot)));
    auto sv = m_compositor.CreateShapeVisual();
    sv.Size({dot, dot});
    sv.Shapes().Append(shape);
    sv.Offset({0, m_m.bottom - (m_m.pillHeight - m_m.restSize) / 2 + dot, 0});
    sv.Opacity(instant && item.running ? DotOpacity() : 0.0f);
    m_root.Children().InsertAtTop(sv);
    e.dot = sv;
    return e;
}

void Dock::SetItems(std::vector<Item> const& items) {
    const bool first = m_entries.empty();
    auto batch = m_compositor.CreateScopedBatch(CompositionBatchTypes::Animation);

    // Icons we already have keep their visuals; the others are new.
    std::vector<Entry> merged;
    std::vector<bool> kept(m_entries.size());
    for (auto const& item : items) {
        size_t j = 0;
        while (j < m_entries.size() && (kept[j] || m_entries[j].key != item.key)) j++;
        if (j == m_entries.size()) {
            merged.push_back(MakeEntry(item, first));
            if (!first) AnimateTo(merged.back().icon.Properties(), L"P", 1.0f, kPresenceMs);
            if (!first && item.running) AnimateTo(merged.back().dot, L"Opacity", DotOpacity(), kFadeMs);
            continue;
        }
        kept[j] = true;
        Entry e = m_entries[j];
        e.extra = item.extra;
        if (e.leaving) {  // came back before it finished shrinking
            e.leaving = false;
            AnimateTo(e.icon.Properties(), L"P", 1.0f, kPresenceMs);
            if (item.running) AnimateTo(e.dot, L"Opacity", DotOpacity(), kFadeMs);
        }
        if (e.running != item.running) {
            e.running = item.running;
            AnimateTo(e.dot, L"Opacity", item.running ? DotOpacity() : 0.0f, kFadeMs);
            if (item.running) Settle(e);  // the app we were bouncing for has opened
        }
        merged.push_back(e);
    }

    // Icons that are gone shrink out where they were, after the one before them.
    size_t after = 0;  // insert position: just past the last icon placed from the old list
    for (size_t j = 0; j < m_entries.size(); j++) {
        if (kept[j]) {
            after = std::find_if(merged.begin(), merged.end(), [&](Entry const& e) { return e.id == m_entries[j].id; }) -
                    merged.begin() + 1;
            continue;
        }
        Entry e = m_entries[j];
        if (!e.leaving) {
            e.leaving = true;
            AnimateTo(e.icon.Properties(), L"P", 0.0f, kPresenceMs);
            AnimateTo(e.dot, L"Opacity", 0.0f, kFadeMs);
        }
        merged.insert(merged.begin() + after, e);
        after++;
    }
    m_entries = std::move(merged);

    // The divider sits before the first unpinned icon; it is there while one of them is.
    int divider = -1;
    bool live = false;
    for (size_t i = 0; i < m_entries.size(); i++) {
        if (!m_entries[i].extra) continue;
        if (divider < 0 && i > 0) divider = static_cast<int>(i);
        live |= !m_entries[i].leaving;
    }
    m_dividerAt = divider;
    bool want = live && divider > 0;
    if (want != m_dividerShown) {
        m_dividerShown = want;
        if (first) {
            m_divider.Properties().InsertScalar(L"Slot", want ? m_m.gap : 0.0f);
            m_divider.Opacity(want ? 1.0f : 0.0f);
        } else {
            AnimateTo(m_divider.Properties(), L"Slot", want ? m_m.gap : 0.0f, kPresenceMs);
            AnimateTo(m_divider, L"Opacity", want ? 1.0f : 0.0f, kPresenceMs);
        }
    }
    batch.End();

    std::vector<unsigned> leaving;
    for (auto const& e : m_entries) if (e.leaving) leaving.push_back(e.id);
    if (!leaving.empty()) {
        // The dock can be rebuilt (settings, theme) before the shrink finishes; the callback must not touch it then.
        batch.Completed([this, alive = std::weak_ptr<bool>(m_alive), leaving](auto&&, auto&&) {
            if (!alive.expired()) Prune(leaving);
        });
    }
    BuildExpressions();
}

// Drops the icons that finished shrinking out (unless they came back).
void Dock::Prune(std::vector<unsigned> const& ids) {
    bool changed = false;
    for (size_t i = m_entries.size(); i-- > 0;) {
        Entry& e = m_entries[i];
        if (!e.leaving || std::find(ids.begin(), ids.end(), e.id) == ids.end()) continue;
        m_root.Children().Remove(e.icon);
        m_root.Children().Remove(e.dot);
        m_entries.erase(m_entries.begin() + i);
        changed = true;
    }
    if (!changed) return;
    m_dividerAt = -1;
    for (size_t i = 1; i < m_entries.size() && m_dividerAt < 0; i++)
        if (m_entries[i].extra) m_dividerAt = static_cast<int>(i);
    if (m_dividerAt < 0) m_divider.Properties().InsertScalar(L"Slot", 0.0f);
    BuildExpressions();
}

void Dock::BuildExpressions() {
    const size_t n = m_entries.size();
    const int d = m_dividerAt;
    // Sum of property `prop` of the icons [0, count).
    auto sum = [&](size_t count, wchar_t const* prop) {
        if (count == 0) return std::wstring(L"0");
        std::wstring s = L"(";
        for (size_t j = 0; j < count; j++) s += (j ? L" + " : L"") + Param(j) + L"." + prop;
        return s + L")";
    };
    // Where icon i's slot starts, relative to the first slot, in terms of `prop`
    // (S: as magnified now; P: at rest). Each slot is Pitch wide times its
    // scale, so neighbours make room for the full magnified size, like macOS.
    auto start = [&](size_t i, wchar_t const* prop) {
        std::wstring s = L"p.Pitch * " + sum(i, prop);
        if (d >= 0 && static_cast<int>(i) >= d) s += L" + dv.Slot";
        return s;
    };
    auto total = [&](wchar_t const* prop) {
        return L"(p.Pitch * " + sum(n, prop) + (d >= 0 ? L" + dv.Slot)" : L")");
    };
    auto bind = [&](ExpressionAnimation const& a) {
        a.SetReferenceParameter(L"p", m_props);
        a.SetReferenceParameter(L"dv", m_divider.Properties());
        for (size_t j = 0; j < n; j++) a.SetReferenceParameter(Param(j), m_entries[j].icon.Properties());
    };

    std::wstring clusterShift = m_shift != 0.0f ? (L" + " + std::to_wstring(m_shift)) : L"";

    for (size_t i = 0; i < n; i++) {
        const std::wstring me = Param(i);
        // Icon i's center at rest, where the wave measures its distance from.
        std::wstring rest = L"(p.ContentCenter" + clusterShift + L" - " + total(L"P") + L" / 2 + " + start(i, L"P") + L" + p.Pitch * " + me +
                            L".P / 2)";
        auto s = m_compositor.CreateExpressionAnimation(
            me + L".P * (1 + (p.MaxScale - 1) * p.Intensity * (Cos(Clamp(Abs(p.PointerX - " + rest +
            L") / p.Radius, 0, 1) * 3.14159265) + 1) / 2)");
        bind(s);
        Start(m_entries[i].icon.Properties(), L"S", s, L"icon magnification");

        auto scale = m_compositor.CreateExpressionAnimation(
            L"Vector3(Max(" + me + L".S, 0.001), Max(" + me + L".S, 0.001), 1)");
        scale.SetReferenceParameter(me, m_entries[i].icon.Properties());
        Start(m_entries[i].icon, L"Scale", scale, L"icon scale");

        // Slots packed around the dock's center; the icon sits centered in
        // its slot (Offset is its top-left before scaling).
        const std::wstring slotCenter = L"p.ContentCenter" + clusterShift + L" - " + total(L"S") + L" / 2 + " + start(i, L"S") + L" + p.Pitch * " +
                                        me + L".S / 2";
        // The magnetic pull: a fraction of the way to the pointer, capped, eased in by Mag (stays 0 under reduce motion).
        const std::wstring pull = L" + Clamp((p.PointerX - (" + slotCenter + L")) * 0.18, -" + std::to_wstring(kPullPx * m_m.restSize / 28) +
                                  L", " + std::to_wstring(kPullPx * m_m.restSize / 28) + L") * " + me + L".Mag";
        auto x = m_compositor.CreateExpressionAnimation(slotCenter + pull + L" - p.Rest / 2");
        bind(x);
        Start(m_entries[i].icon, L"Offset.X", x, L"icon x");

        auto dx = m_compositor.CreateExpressionAnimation(slotCenter + L" - " + std::to_wstring(DotSize() / 2));
        bind(dx);
        Start(m_entries[i].dot, L"Offset.X", dx, L"dot x");
    }

    if (d >= 0) {
        auto dx = m_compositor.CreateExpressionAnimation(L"p.ContentCenter" + clusterShift + L" - " + total(L"S") + L" / 2 + p.Pitch * " +
                                                         sum(d, L"S") + L" + dv.Slot / 2 - " +
                                                         std::to_wstring(m_divider.Size().x / 2));
        bind(dx);
        Start(m_divider, L"Offset.X", dx, L"divider x");
    }

    if (m_cluster && m_clusterDivider) {
        auto clusterX = m_compositor.CreateExpressionAnimation(L"pill.Offset.X + p.Padding");
        clusterX.SetReferenceParameter(L"pill", m_pill);
        clusterX.SetReferenceParameter(L"p", m_props);
        Start(m_cluster->Visual(), L"Offset.X", clusterX, L"cluster x");

        auto clusterDivX = m_compositor.CreateExpressionAnimation(
            L"pill.Offset.X + p.Padding + " + std::to_wstring(m_clusterWidth + m_dividerWidth / 2 - m_clusterDivider.Size().x / 2));
        clusterDivX.SetReferenceParameter(L"pill", m_pill);
        clusterDivX.SetReferenceParameter(L"p", m_props);
        Start(m_clusterDivider, L"Offset.X", clusterDivX, L"cluster divider x");
    }

    if (m_widgets && m_widgets->Capsule()) {
        // Glued to the pill's right edge (and so follows it as the wave stretches it), one gap away.
        auto cx = m_compositor.CreateExpressionAnimation(L"pill.Offset.X + pill.Size.X + " + std::to_wstring(m_widgets->CapsuleGap()));
        cx.SetReferenceParameter(L"pill", m_pill);
        Start(m_widgets->Capsule(), L"Offset.X", cx, L"capsule x");
    }

    // The pill wraps the slots: it grows with the icons instead of letting
    // the outer ones slide off.
    if (n > 0 || m_cluster || m_widgets) {
        std::wstring widthExpr = (m_clusterOffset > 0.0f ? (std::to_wstring(m_clusterOffset) + L" + ") : L"") +
                                 total(L"S") + L" - p.Gap + 2 * p.Padding";
        auto width = m_compositor.CreateExpressionAnimation(widthExpr);
        bind(width);
        Start(m_pill, L"Size.X", width, L"pill width");

        auto left = m_compositor.CreateExpressionAnimation(L"p.ContentCenter - pill.Size.X / 2");
        left.SetReferenceParameter(L"p", m_props);
        left.SetReferenceParameter(L"pill", m_pill);
        Start(m_pill, L"Offset.X", left, L"pill x");

        auto clip = m_pill.Clip().as<CompositionGeometricClip>();
        auto clipSize = m_compositor.CreateExpressionAnimation(L"pill.Size");
        clipSize.SetReferenceParameter(L"pill", m_pill);
        Start(clip.Geometry(), L"Size", clipSize, L"clip size");
    }
}

void Dock::OnPointerMove(float x) {
    m_pointerX = x;
    m_props.InsertScalar(L"PointerX", x);
    if (!m_hovering) {
        m_hovering = true;
        AnimateTo(m_props, L"Intensity", 1.0f, kFocusMs);
    }
}

void Dock::OnPointerLeave() {
    SetMagnet(-1);
    if (!m_hovering) return;
    m_hovering = false;
    AnimateTo(m_props, L"Intensity", 0.0f, kFocusMs);
}

std::pair<float, float> Dock::RestIconCenter(size_t index) const {
    std::vector<Entry const*> live;
    for (auto const& e : m_entries) if (!e.leaving) live.push_back(&e);
    const size_t n = live.size();
    int d = -1;
    for (size_t k = 1; k < n && d < 0; k++) if (live[k]->extra) d = static_cast<int>(k);
    const float pitch = m_m.restSize + m_m.gap;
    const float divider = d >= 0 ? m_m.gap : 0.0f;
    // The rest layout HitTest uses, with every icon fully present.
    const float left = (m_m.centerX - CapsuleExtent() / 2) + m_shift - (pitch * n + divider) / 2;
    const float x = left + pitch * index + (d >= 0 && static_cast<int>(index) >= d ? divider : 0.0f) + pitch / 2;
    return {x, IconY() + m_m.restSize / 2};
}

std::pair<float, float> Dock::MagnifiedIconCenterAndTop(size_t index) const {
    std::vector<Entry const*> live;
    for (auto const& e : m_entries) if (!e.leaving) live.push_back(&e);
    const size_t n = live.size();
    if (index >= n) return {0.0f, 0.0f};

    int d = -1;
    for (size_t k = 1; k < n && d < 0; k++) if (live[k]->extra) d = static_cast<int>(k);
    const float pitch = m_m.restSize + m_m.gap;
    const float divider = d >= 0 ? m_m.gap : 0.0f;
    const float restLeft = (m_m.centerX - CapsuleExtent() / 2) + m_shift - (pitch * n + divider) / 2;
    std::vector<float> mag(n);
    float total = divider;
    for (size_t k = 0; k < n; k++) {
        float center = restLeft + pitch * k + (d >= 0 && static_cast<int>(k) >= d ? divider : 0.0f) + pitch / 2;
        float t = std::min(std::abs(m_pointerX - center) / m_m.radius, 1.0f);
        mag[k] = 1 + (m_m.maxScale - 1) * (m_hovering ? 1.0f : 0.0f) * (std::cos(t * kPi) + 1) / 2;
        total += pitch * mag[k];
    }
    float left = (m_m.centerX - CapsuleExtent() / 2) + m_shift - total / 2;
    for (size_t k = 0; k < index; k++) {
        if (static_cast<int>(k) == d) left += divider;
        left += pitch * mag[k];
    }
    if (static_cast<int>(index) == d) left += divider;
    float slot = pitch * mag[index];
    float centerX = left + slot / 2;
    float topY = m_m.bottom - (m_m.pillHeight - m_m.restSize) / 2 - m_m.restSize * mag[index];
    return {centerX, topY};
}

Dock::Target Dock::HitTarget(float x, float y) const {
    std::vector<Entry const*> live;
    for (auto const& e : m_entries) if (!e.leaving) live.push_back(&e);
    const size_t n = live.size();

    int d = -1;
    for (size_t k = 1; k < n && d < 0; k++) if (live[k]->extra) d = static_cast<int>(k);
    const float pitch = m_m.restSize + m_m.gap;
    const float divider = d >= 0 ? m_m.gap : 0.0f;
    const float restLeft = (m_m.centerX - CapsuleExtent() / 2) + m_shift - (pitch * n + divider) / 2;
    std::vector<float> mag(n);
    float total = divider;
    for (size_t k = 0; k < n; k++) {
        float center = restLeft + pitch * k + (d >= 0 && static_cast<int>(k) >= d ? divider : 0.0f) + pitch / 2;
        float t = std::min(std::abs(m_pointerX - center) / m_m.radius, 1.0f);
        mag[k] = 1 + (m_m.maxScale - 1) * (m_hovering ? 1.0f : 0.0f) * (std::cos(t * kPi) + 1) / 2;
        total += pitch * mag[k];
    }

    // The pill's width as the expressions compute it, with every icon fully present.
    const float pillWidth = m_clusterOffset + (n > 0 ? total - m_m.gap : 0.0f) + 2 * m_m.padding;
    const float pillLeft = (m_m.centerX - CapsuleExtent() / 2) - pillWidth / 2;
    const float pillTop = m_m.bottom - m_m.pillHeight;

    // The targets left to right, as horizontal spans: the cluster (all of it with HALO 2.0, else the Start logo),
    // then every icon's whole slot, then the widgets.
    std::vector<halo::hit::Span> spans;
    std::vector<Target> targets;
    if (m_cluster) {
        const float cl = pillLeft + m_m.padding;
        if (m_h2) spans.push_back({cl, cl + m_clusterWidth});
        else spans.push_back({cl + m_cluster->StartX(), cl + m_cluster->StartX() + m_cluster->StartWidth()});
        targets.push_back({Kind::Start, -1});
    }
    float left = (m_m.centerX - CapsuleExtent() / 2) + m_shift - total / 2;
    for (size_t k = 0; k < n; k++) {
        if (static_cast<int>(k) == d) left += divider;
        const float slot = pitch * mag[k];
        spans.push_back({left, left + slot});
        targets.push_back({Kind::App, static_cast<int>(k)});
        left += slot;
    }
    if (m_widgets && m_widgets->CapsuleShown()) {  // the Now Playing pill beside the dock: artwork and text, then its two buttons
        using Part = DockWidgets::Part;
        const float cl = pillLeft + pillWidth + m_widgets->CapsuleGap();
        for (auto [part, kind] : {std::pair{Part::Body, Kind::Media}, {Part::Play, Kind::MediaPlay}, {Part::Next, Kind::MediaNext}}) {
            spans.push_back({cl + m_widgets->PartLeft(part), cl + m_widgets->PartRight(part)});
            targets.push_back({kind, -1});
        }
    }
    // Reaches the pill's edge (the padding) and half a gap beyond; the gaps between targets are at most one gap.
    const int hit = halo::hit::Nearest(spans, x, m_m.padding + m_m.gap / 2);
    if (hit < 0) return {};
    Target t = targets[hit];
    // A little above the pill (or the magnified icon) still counts.
    float top = pillTop;
    if (t.kind == Kind::App) top = std::min(top, m_m.bottom - (m_m.pillHeight - m_m.restSize) / 2 - m_m.restSize * mag[t.index]);
    if (!halo::hit::BelowTop(y, top, m_m.restSize / 2)) return {};
    return t;
}

bool Dock::HitStart(float x, float y) const { return HitTarget(x, y).kind == Kind::Start; }

int Dock::HitTest(float x, float y) const {
    Target t = HitTarget(x, y);
    return t.kind == Kind::App ? t.index : -1;
}

// The hovered icon is pulled toward the pointer by the compositor (the Mag term in BuildExpressions); this only
// eases which icon's Mag is up.
void Dock::SetMagnet(int index) {
    if (halo::settings::Get().reduceMotion) index = -1;
    unsigned want = 0;
    int k = 0;
    for (auto const& e : m_entries) {
        if (e.leaving) continue;
        if (k++ == index) want = e.id;
    }
    if (want == m_magnetId) return;
    for (auto& e : m_entries) {
        if (e.id == m_magnetId && m_magnetId) AnimateTo(e.icon.Properties(), L"Mag", 0.0f, kMagnetMs);
        if (e.id == want) AnimateTo(e.icon.Properties(), L"Mag", 1.0f, kMagnetMs);
    }
    m_magnetId = want;
}

void Dock::SetPressed(int index) {
    if (m_overlay) {
        auto old = m_overlay;
        m_overlay = nullptr;
        auto batch = m_compositor.CreateScopedBatch(CompositionBatchTypes::Animation);
        AnimateTo(old, L"Opacity", 0.0f, kPressOutMs);
        batch.End();
        batch.Completed([old](auto&&, auto&&) { old.Parent().Children().Remove(old); });
    }
    int k = 0;
    for (auto const& e : m_entries) {
        if (e.leaving || k++ != index) continue;
        // The icon's own alpha masks a black layer, so only the icon darkens.
        auto mask = m_compositor.CreateMaskBrush();
        mask.Source(m_compositor.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0, 0, 0)));
        mask.Mask(e.brush);
        m_overlay = m_compositor.CreateSpriteVisual();
        m_overlay.RelativeSizeAdjustment({1, 1});
        m_overlay.Brush(mask);
        m_overlay.Opacity(0.0f);
        e.icon.Children().InsertAtTop(m_overlay);
        AnimateTo(m_overlay, L"Opacity", kPressDarkness, kPressInMs);
        break;
    }
}

bool Dock::Bouncing(Entry const& e) const {
    return e.bounceStart != std::chrono::steady_clock::time_point{} &&
           std::chrono::steady_clock::now() - e.bounceStart < std::chrono::milliseconds(kBounceMs * kBounces);
}

void Dock::Bounce(int index) {
    int k = 0;
    for (auto& e : m_entries) {
        if (e.leaving || k++ != index || Bouncing(e)) continue;
        e.bounceStart = std::chrono::steady_clock::now();
        fwprintf(stderr, L"[halo] bounce started\n"); fflush(stderr);
        // Up by half the rest size, easing out on the way up and in on the way down.
        const float rest = IconY();
        auto a = m_compositor.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(0.0f, rest);
        a.InsertKeyFrame(0.5f, rest - m_m.restSize / 2, BounceUp(m_compositor));
        a.InsertKeyFrame(1.0f, rest, BounceDown(m_compositor));
        a.Duration(std::chrono::milliseconds(kBounceMs));
        a.IterationCount(kBounces);
        Start(e.icon, L"Offset.Y", a, L"bounce");
        break;
    }
}

void Dock::Bob(int index) {
    int k = 0;
    for (auto& e : m_entries) {
        if (e.leaving || k++ != index || Bouncing(e)) continue;
        const float rest = IconY();
        auto a = m_compositor.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(0.0f, rest);
        a.InsertKeyFrame(0.45f, rest - m_m.restSize * 0.4f, BounceUp(m_compositor));
        a.InsertKeyFrame(1.0f, rest, BounceDown(m_compositor));
        a.Duration(std::chrono::milliseconds(kBounceMs * 2 / 3));
        Start(e.icon, L"Offset.Y", a, L"bob");
        fwprintf(stderr, L"[halo] bob started\n"); fflush(stderr);
        break;
    }
}

// The app opened: like macOS, finish the bounce it is in rather than cutting it short. The same
// curves as Bounce for what is left of this cycle, from wherever the icon is now; then it rests.
void Dock::Settle(Entry& e) {
    if (!Bouncing(e)) return;
    const float elapsed =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - e.bounceStart).count();
    const float phase = std::fmod(elapsed, static_cast<float>(kBounceMs)) / kBounceMs;
    e.bounceStart = {};
    auto a = m_compositor.CreateScalarKeyFrameAnimation();
    if (phase < 0.5f)  // still rising: on up to the top first
        a.InsertKeyFrame((0.5f - phase) / (1 - phase), IconY() - m_m.restSize / 2, BounceUp(m_compositor));
    a.InsertKeyFrame(1.0f, IconY(), BounceDown(m_compositor));
    a.Duration(std::chrono::milliseconds(std::max(1, static_cast<int>((1 - phase) * kBounceMs))));
    Start(e.icon, L"Offset.Y", a, L"settle");
}

void Dock::OnClockTimer() {
    if (m_cluster) m_cluster->OnTimer(DockCluster::kClockTimer);
}

void Dock::OnTimeChange() {
    if (m_cluster) m_cluster->OnTimeChange();
}

// Takes the dock's visuals out of its parent, before a rebuild (no destructor: it can outlive the compositor).
void Dock::Detach() {
    if (m_root && m_root.Parent()) m_root.Parent().Children().Remove(m_root);
}

void Dock::OnSettingChange() {
    if (m_cluster) m_cluster->OnSettingChange();
}

void Dock::OnPowerBroadcast(WPARAM w, LPARAM l) {
    if (m_cluster) m_cluster->OnPowerBroadcast(w, l);
}

void Dock::OnNetworkChanged() {
    if (m_cluster) m_cluster->OnNetworkChanged();
  }
