#include "dock.h"

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
constexpr int kFocusMs = 150;      // magnification in and out
constexpr int kPresenceMs = 250;   // an icon growing in or shrinking out
constexpr int kFadeMs = 150;       // running dots
constexpr int kPressInMs = 60;
constexpr int kPressOutMs = 100;
constexpr float kPressDarkness = 0.3f;
constexpr int kBounceMs = 350;     // one bounce, up and back down
constexpr int kBounces = 3;
constexpr float kDotOpacity = 0.7f;

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

// Expression parameter name of icon j's property set.
std::wstring Param(size_t j) { return L"s" + std::to_wstring(j); }

// Width of the pill for `count` icons at rest.
float WidthAtRest(DockMetrics const& m, size_t count, bool divider) {
    return static_cast<float>(count) * (m.restSize + m.gap) - m.gap + 2 * m.padding + (divider ? m.gap : 0.0f);
}

}  // namespace

Dock::Dock(Compositor const& compositor, ContainerVisual const& parent,
           ContainerVisual const& pill, DockMetrics const& metrics)
    : m_compositor(compositor), m_pill(pill), m_m(metrics) {
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

    // A 1px line, 60% of the pill high, in a slot one gap wide: a full gap
    // each side. Its slot and opacity animate when the first unpinned icon
    // arrives and the last one leaves.
    const float w = std::max(1.0f, std::round(m_m.restSize / 28));
    const float h = m_m.pillHeight * 0.6f;
    m_divider = m_compositor.CreateSpriteVisual();
    m_divider.Size({w, h});
    m_divider.Brush(m_compositor.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0x33, 0xFF, 0xFF, 0xFF)));
    m_divider.Offset({0, m_m.bottom - (m_m.pillHeight + h) / 2, 0});
    m_divider.Opacity(0.0f);
    m_divider.Properties().InsertScalar(L"Slot", 0.0f);
    m_root.Children().InsertAtTop(m_divider);

    m_pill.Size({WidthAtRest(m_m, 0, false), m_m.pillHeight});
    m_pill.Offset({m_m.centerX - m_pill.Size().x / 2, m_m.bottom - m_m.pillHeight, 0});
    auto clipShape = m_compositor.CreateRoundedRectangleGeometry();
    clipShape.CornerRadius({m_m.cornerRadius, m_m.cornerRadius});
    clipShape.Size(m_pill.Size());
    m_pill.Clip(m_compositor.CreateGeometricClip(clipShape));
    m_root.Children().InsertAtBottom(m_pill);
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
    return WidthAtRest(m_m, n, extra);
}

float Dock::MaxWidth() const {
    // The wave adds at most about 2 * radius / pitch icons' worth of growth.
    float pitch = m_m.restSize + m_m.gap;
    float growth = 0.0f;
    for (float d = -m_m.radius; d <= m_m.radius; d += pitch) {
        growth += (m_m.maxScale - 1) * (std::cos(std::abs(d) / m_m.radius * kPi) + 1) / 2;
    }
    return WidthAtRest(m_m, m_entries.size(), true) + (growth + 1) * pitch;
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
    m_root.Children().InsertAtTop(v);
    e.icon = v;

    // The running dot sits one dot-size under the icon and stays put.
    const float dot = DotSize();
    auto geometry = m_compositor.CreateEllipseGeometry();
    geometry.Radius({dot / 2, dot / 2});
    auto shape = m_compositor.CreateSpriteShape(geometry);
    shape.Offset({dot / 2, dot / 2});
    shape.FillBrush(m_compositor.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0xFF, 0xFF, 0xFF, 0xFF)));
    auto sv = m_compositor.CreateShapeVisual();
    sv.Size({dot, dot});
    sv.Shapes().Append(shape);
    sv.Offset({0, m_m.bottom - (m_m.pillHeight - m_m.restSize) / 2 + dot, 0});
    sv.Opacity(instant && item.running ? kDotOpacity : 0.0f);
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
            if (!first && item.running) AnimateTo(merged.back().dot, L"Opacity", kDotOpacity, kFadeMs);
            continue;
        }
        kept[j] = true;
        Entry e = m_entries[j];
        e.extra = item.extra;
        if (e.leaving) {  // came back before it finished shrinking
            e.leaving = false;
            AnimateTo(e.icon.Properties(), L"P", 1.0f, kPresenceMs);
            if (item.running) AnimateTo(e.dot, L"Opacity", kDotOpacity, kFadeMs);
        }
        if (e.running != item.running) {
            e.running = item.running;
            AnimateTo(e.dot, L"Opacity", item.running ? kDotOpacity : 0.0f, kFadeMs);
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
        batch.Completed([this, leaving](auto&&, auto&&) { Prune(leaving); });
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

    for (size_t i = 0; i < n; i++) {
        const std::wstring me = Param(i);
        // Icon i's center at rest, where the wave measures its distance from.
        std::wstring rest = L"(p.Center - " + total(L"P") + L" / 2 + " + start(i, L"P") + L" + p.Pitch * " + me +
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
        const std::wstring slotCenter = L"p.Center - " + total(L"S") + L" / 2 + " + start(i, L"S") + L" + p.Pitch * " +
                                        me + L".S / 2";
        auto x = m_compositor.CreateExpressionAnimation(slotCenter + L" - p.Rest / 2");
        bind(x);
        Start(m_entries[i].icon, L"Offset.X", x, L"icon x");

        auto dx = m_compositor.CreateExpressionAnimation(slotCenter + L" - " + std::to_wstring(DotSize() / 2));
        bind(dx);
        Start(m_entries[i].dot, L"Offset.X", dx, L"dot x");
    }

    if (d >= 0) {
        auto dx = m_compositor.CreateExpressionAnimation(L"p.Center - " + total(L"S") + L" / 2 + p.Pitch * " +
                                                         sum(d, L"S") + L" + dv.Slot / 2 - " +
                                                         std::to_wstring(m_divider.Size().x / 2));
        bind(dx);
        Start(m_divider, L"Offset.X", dx, L"divider x");
    }

    // The pill wraps the slots: it grows with the icons instead of letting
    // the outer ones slide off.
    if (n > 0) {
        auto width = m_compositor.CreateExpressionAnimation(total(L"S") + L" - p.Gap + 2 * p.Padding");
        bind(width);
        Start(m_pill, L"Size.X", width, L"pill width");

        auto left = m_compositor.CreateExpressionAnimation(L"p.Center - pill.Size.X / 2");
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
    const float left = m_m.centerX - (pitch * n + divider) / 2;
    const float x = left + pitch * index + (d >= 0 && static_cast<int>(index) >= d ? divider : 0.0f) + pitch / 2;
    return {x, IconY() + m_m.restSize / 2};
}

int Dock::HitTest(float x, float y) const {
    std::vector<Entry const*> live;
    for (auto const& e : m_entries) if (!e.leaving) live.push_back(&e);
    const size_t n = live.size();
    if (n == 0) return -1;
    int d = -1;
    for (size_t k = 1; k < n && d < 0; k++) if (live[k]->extra) d = static_cast<int>(k);
    const float pitch = m_m.restSize + m_m.gap;
    const float divider = d >= 0 ? m_m.gap : 0.0f;
    // The same layout the expressions compute, with every icon fully present.
    const float restLeft = m_m.centerX - (pitch * n + divider) / 2;
    std::vector<float> mag(n);
    float total = divider;
    for (size_t k = 0; k < n; k++) {
        float center = restLeft + pitch * k + (d >= 0 && static_cast<int>(k) >= d ? divider : 0.0f) + pitch / 2;
        float t = std::min(std::abs(m_pointerX - center) / m_m.radius, 1.0f);
        mag[k] = 1 + (m_m.maxScale - 1) * (m_hovering ? 1.0f : 0.0f) * (std::cos(t * kPi) + 1) / 2;
        total += pitch * mag[k];
    }
    float left = m_m.centerX - total / 2;
    for (size_t k = 0; k < n; k++) {
        if (static_cast<int>(k) == d) left += divider;
        float slot = pitch * mag[k];
        if (x >= left && x < left + slot) {
            float top = m_m.bottom - (m_m.pillHeight - m_m.restSize) / 2 - m_m.restSize * mag[k];
            return y >= std::min(top, m_m.bottom - m_m.pillHeight) ? static_cast<int>(k) : -1;
        }
        left += slot;
    }
    return -1;
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
        // Up by half the rest size, easing out on the way up and in on the way down.
        const float rest = IconY();
        auto a = m_compositor.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(0.0f, rest);
        a.InsertKeyFrame(0.5f, rest - m_m.restSize / 2,
                         m_compositor.CreateCubicBezierEasingFunction({0.3f, 0.5f}, {0.5f, 1.0f}));
        a.InsertKeyFrame(1.0f, rest, m_compositor.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.7f, 0.5f}));
        a.Duration(std::chrono::milliseconds(kBounceMs));
        a.IterationCount(kBounces);
        Start(e.icon, L"Offset.Y", a, L"bounce");
        break;
    }
}

// The app opened: let the icon come down from wherever the bounce has it.
void Dock::Settle(Entry& e) {
    if (!Bouncing(e)) return;
    e.bounceStart = {};
    auto a = m_compositor.CreateScalarKeyFrameAnimation();
    a.InsertKeyFrame(1.0f, IconY(), m_easeOut);
    a.Duration(std::chrono::milliseconds(120));
    Start(e.icon, L"Offset.Y", a, L"settle");
}
