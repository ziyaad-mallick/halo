#include "dock.h"

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

// The magnification of icon i as a compositor expression: 1 at rest, up to
// MaxScale under the cursor, with a cosine falloff over Radius px.
std::wstring MagnificationExpr(float restCenter) {
    return L"(1 + (p.MaxScale - 1) * p.Intensity * (Cos(Clamp(Abs(p.PointerX - " +
           std::to_wstring(restCenter) + L") / p.Radius, 0, 1) * 3.14159265) + 1) / 2)";
}

// Sum of the slot widths of icons [0, count): each slot is Pitch * scale, so
// neighbours make room for the full magnified size, like macOS.
std::wstring SlotSumExpr(size_t count) {
    if (count == 0) return L"0";
    std::wstring s = L"p.Pitch * (";
    for (size_t j = 0; j < count; j++) {
        if (j) s += L" + ";
        s += L"v" + std::to_wstring(j) + L".Scale.X";
    }
    return s + L")";
}

}  // namespace

Dock::Dock(Compositor const& compositor, ContainerVisual const& parent,
           ContainerVisual const& pill, DockMetrics const& metrics)
    : m_compositor(compositor), m_pill(pill), m_m(metrics) {
    m_root = m_compositor.CreateContainerVisual();
    m_root.RelativeSizeAdjustment({1.0f, 1.0f});
    parent.Children().InsertAtTop(m_root);

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

    m_pill.Size({RestWidth(), m_m.pillHeight});
    m_pill.Offset({m_m.centerX - RestWidth() / 2, m_m.bottom - m_m.pillHeight, 0});
    auto clipShape = m_compositor.CreateRoundedRectangleGeometry();
    clipShape.CornerRadius({m_m.cornerRadius, m_m.cornerRadius});
    clipShape.Size(m_pill.Size());
    m_pill.Clip(m_compositor.CreateGeometricClip(clipShape));
    m_root.Children().InsertAtBottom(m_pill);
}

float Dock::RestWidth() const {
    float n = static_cast<float>(m_icons.size());
    return n * (m_m.restSize + m_m.gap) - m_m.gap + 2 * m_m.padding;
}

float Dock::MaxWidth() const {
    // The wave adds at most about 2 * radius / pitch icons' worth of growth.
    float pitch = m_m.restSize + m_m.gap;
    float growth = 0.0f;
    for (float d = -m_m.radius; d <= m_m.radius; d += pitch) {
        growth += (m_m.maxScale - 1) * (std::cos(std::abs(d) / m_m.radius * kPi) + 1) / 2;
    }
    return RestWidth() + growth * pitch;
}

float Dock::RestCenter(size_t i) const {
    float pitch = m_m.restSize + m_m.gap;
    float total = static_cast<float>(m_icons.size()) * pitch;
    return m_m.centerX - total / 2 + (static_cast<float>(i) + 0.5f) * pitch;
}

float Dock::MagnificationAt(float pointerX, size_t i) const {
    float t = std::min(std::abs(pointerX - RestCenter(i)) / m_m.radius, 1.0f);
    float intensity = m_hovering ? 1.0f : 0.0f;
    return 1 + (m_m.maxScale - 1) * intensity * (std::cos(t * kPi) + 1) / 2;
}

float Dock::DotSize() const { return std::max(2.0f, std::round(m_m.restSize / 11)); }

void Dock::SetCenter(float x) {
    m_m.centerX = x;
    m_props.InsertScalar(L"Center", x);
}

void Dock::SetIcons(std::vector<Item> const& items, size_t divider) {
    for (auto& v : m_icons) m_root.Children().Remove(v);
    for (auto& d : m_dots) if (d) m_root.Children().Remove(d);
    if (m_divider) m_root.Children().Remove(m_divider);
    m_icons.clear();
    m_dots.clear();
    m_divider = nullptr;
    const float vPad = (m_m.pillHeight - m_m.restSize) / 2;
    const float dot = DotSize();
    for (auto const& item : items) {
        auto v = m_compositor.CreateSpriteVisual();
        v.Brush(item.brush);
        v.Size({m_m.restSize, m_m.restSize});
        // Grow upward from the bottom-center, like the macOS dock.
        v.CenterPoint({m_m.restSize / 2, m_m.restSize, 0});
        v.Offset({0, m_m.bottom - vPad - m_m.restSize, 0});
        m_root.Children().InsertAtTop(v);
        m_icons.push_back(v);

        // The running dot sits in the padding under the icon and stays put.
        Visual d{nullptr};
        if (item.running) {
            auto geometry = m_compositor.CreateEllipseGeometry();
            geometry.Radius({dot / 2, dot / 2});
            auto shape = m_compositor.CreateSpriteShape(geometry);
            shape.Offset({dot / 2, dot / 2});
            shape.FillBrush(m_compositor.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0xD0, 0xFF, 0xFF, 0xFF)));
            auto sv = m_compositor.CreateShapeVisual();
            sv.Size({dot, dot});
            sv.Shapes().Append(shape);
            sv.Offset({0, m_m.bottom - vPad / 2 - dot / 2, 0});
            m_root.Children().InsertAtTop(sv);
            d = sv;
        }
        m_dots.push_back(d);
    }
    m_dividerAt = divider > 0 && divider < items.size() ? divider : 0;
    if (m_dividerAt) {
        float w = std::max(1.0f, std::round(m_m.restSize / 44));
        float h = m_m.restSize * 0.9f;
        m_divider = m_compositor.CreateSpriteVisual();
        m_divider.Size({w, h});
        m_divider.Brush(m_compositor.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0x50, 0xFF, 0xFF, 0xFF)));
        m_divider.Offset({0, m_m.bottom - vPad - m_m.restSize + (m_m.restSize - h) / 2, 0});
        m_root.Children().InsertAtTop(m_divider);
    }
    BuildExpressions();
}

void Dock::BuildExpressions() {
    const size_t n = m_icons.size();
    auto bindAll = [&](ExpressionAnimation const& a) {
        a.SetReferenceParameter(L"p", m_props);
        for (size_t j = 0; j < n; j++) {
            a.SetReferenceParameter(L"v" + std::to_wstring(j), m_icons[j]);
        }
    };

    const std::wstring total = SlotSumExpr(n);
    for (size_t i = 0; i < n; i++) {
        auto scale = m_compositor.CreateExpressionAnimation(
            L"Vector3(" + MagnificationExpr(RestCenter(i)) + L", " +
            MagnificationExpr(RestCenter(i)) + L", 1)");
        scale.SetReferenceParameter(L"p", m_props);
        Start(m_icons[i], L"Scale", scale, L"icon scale");

        // Slots packed around the dock's center; the icon sits centered in
        // its slot (Offset is its top-left before scaling).
        auto x = m_compositor.CreateExpressionAnimation(
            L"p.Center - (" + total + L") / 2 + " + SlotSumExpr(i) + L" + p.Pitch * v" +
            std::to_wstring(i) + L".Scale.X / 2 - p.Rest / 2");
        bindAll(x);
        Start(m_icons[i], L"Offset.X", x, L"icon x");

        if (m_dots[i]) {
            auto dx = m_compositor.CreateExpressionAnimation(
                L"p.Center - (" + total + L") / 2 + " + SlotSumExpr(i) + L" + p.Pitch * v" +
                std::to_wstring(i) + L".Scale.X / 2 - " + std::to_wstring(DotSize() / 2));
            bindAll(dx);
            Start(m_dots[i], L"Offset.X", dx, L"dot x");
        }
    }

    // The divider sits on the boundary between the last pinned slot and the first extra one.
    if (m_divider) {
        auto dx = m_compositor.CreateExpressionAnimation(
            L"p.Center - (" + total + L") / 2 + " + SlotSumExpr(m_dividerAt) + L" - " +
            std::to_wstring(m_divider.Size().x / 2));
        bindAll(dx);
        Start(m_divider, L"Offset.X", dx, L"divider x");
    }

    // The pill wraps the slots: it grows with the icons instead of letting
    // the outer ones slide off.
    if (n > 0) {
        auto width = m_compositor.CreateExpressionAnimation(
            L"(" + total + L") - p.Gap + 2 * p.Padding");
        bindAll(width);
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

void Dock::AnimateIntensity(float to) {
    auto a = m_compositor.CreateScalarKeyFrameAnimation();
    a.InsertKeyFrame(1.0f, to,
                     m_compositor.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f}));
    a.Duration(std::chrono::milliseconds(to > 0 ? 160 : 220));
    Start(m_props, L"Intensity", a, L"intensity");
}

void Dock::OnPointerMove(float x) {
    m_pointerX = x;
    m_props.InsertScalar(L"PointerX", x);
    if (!m_hovering) {
        m_hovering = true;
        AnimateIntensity(1.0f);
    }
}

void Dock::OnPointerLeave() {
    if (!m_hovering) return;
    m_hovering = false;
    AnimateIntensity(0.0f);
}

int Dock::HitTest(float x, float y) const {
    const size_t n = m_icons.size();
    if (n == 0) return -1;
    float pitch = m_m.restSize + m_m.gap;
    float total = 0;
    for (size_t i = 0; i < n; i++) total += pitch * MagnificationAt(m_pointerX, i);
    float left = m_m.centerX - total / 2;
    for (size_t i = 0; i < n; i++) {
        float slot = pitch * MagnificationAt(m_pointerX, i);
        if (x >= left && x < left + slot) {
            float top = m_m.bottom - m_m.pillHeight - m_m.restSize * (MagnificationAt(m_pointerX, i) - 1);
            return y >= top ? static_cast<int>(i) : -1;
        }
        left += slot;
    }
    return -1;
}
