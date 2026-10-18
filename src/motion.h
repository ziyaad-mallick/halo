// Shared open/close motion for Halo's panels (Control Center, Notification Center, launcher): the dock's
// own reveal, a slide and nothing else. All of it runs on the compositor: one StartAnimation, no timers,
// no per-frame CPU.
//
// A short rise and a fade (tokens.h kEnterTravel): a long slide out of the screen edge covered ~750 px in
// ~180 ms, 100+ px a frame, which read as a lurch even at 60 fps. No Scale: it resamples the whole panel.
//
// In:  springs from rest + travel to rest with the dock's reveal spring, fading in over the first part
//      (decelerating). Out: accelerates back along `travel` and fades. kOutMs long; destroy or hide the
//      window after it (a timer of kOutMs + 10 is fine).
//
// The six effects (docs/SETTINGS.md) are picked per surface by its settings::Surface: rise (above), bounce
// (2x travel, looser spring), swipe (travel along x), fade, swoop (Scale from 92 %, anchored at the corner the
// panel opens from), none. `speed` divides every duration and spring period, `motion` multiplies travel and
// overshoot. OutMs() is the close duration to time the hide against.
//
// Give the window kPad logical px of empty margin around the visual so the overshoot is never clipped.
#pragma once
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include "settings.h"

namespace halo::motion {

constexpr float kDamping = 0.72f;  // the dock's reveal spring (halo.cpp kSpringDamping)
constexpr int kPeriodMs = 55;      // halo.cpp kSpringPeriodMs
constexpr unsigned kOutMs = 160;
constexpr unsigned kFadeInMs = 180;
constexpr float kPad = 16.0f;  // logical px of margin a panel window keeps around its visual

namespace detail {
namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
inline winrt::Windows::Foundation::IReference<float3> Ref3(float3 v) { return v; }  // spring animations take IReference<float3>
// A panel reopened mid-close may still carry the old Scale: put it back at rest.
inline void Settle(wuc::Visual const& v) {
    v.StopAnimation(L"Scale");
    v.Scale({1, 1, 1});
}
constexpr float kSwoopFrom = 0.92f;
inline std::chrono::milliseconds Ms(float ms, float speed) {
    return std::chrono::milliseconds(std::max(1, static_cast<int>(std::lround(ms / speed))));
}
// Where an effect starts from rest: the offset, whether it fades, whether it scales, the spring's damping.
struct Pose {
    float2 off{0, 0};
    bool fade = true, scale = false;
    float damping = 0.72f;
};
inline Pose PoseOf(settings::Surface const& st, float2 travel) {
    using E = settings::Effect;
    const float amt = settings::Amount(st);
    Pose p;
    switch (st.effect) {
        case E::Rise: p.off = {travel.x * amt, travel.y * amt}; break;
        case E::Bounce:  // twice the travel, and a spring that overshoots more the more `motion` there is
            p.off = {travel.x * 2 * amt, travel.y * 2 * amt};
            p.damping = 1.0f - 0.5f * std::min(amt, 1.4f);
            break;
        case E::Swipe: p.off = {std::max(std::abs(travel.x), std::abs(travel.y)) * 2 * amt, 0}; break;
        case E::Swoop: p.off = {travel.x * amt, travel.y * amt}; p.scale = true; break;
        default: break;  // fade, none
    }
    return p;
}
}  // namespace detail

// How long Out() runs for this surface (0 for `none`); time the hide against OutMs(st) + 10.
inline unsigned OutMs(settings::Surface const& st) {
    return st.effect == settings::Effect::None ? 0u : static_cast<unsigned>(detail::Ms(static_cast<float>(kOutMs), settings::Speed(st)).count());
}

// `corner` is the fraction of the visual's Size the swoop grows from ({1,1} = bottom right, {0.5,1} = bottom centre).
inline void In(detail::wuc::Compositor const& c, detail::wuc::Visual const& v, detail::float3 rest, detail::float2 travel,
               settings::Surface const& st = {}, detail::float2 corner = {1, 1}) {
    using namespace std::chrono;
    detail::Settle(v);
    v.StopAnimation(L"Offset");
    if (st.effect == settings::Effect::None) {
        v.StopAnimation(L"Opacity");
        v.Offset(rest);
        v.Opacity(1);
        return;
    }
    const float speed = settings::Speed(st);
    const auto pose = detail::PoseOf(st, travel);
    const detail::float3 from{rest.x + pose.off.x, rest.y + pose.off.y, rest.z};
    v.Offset(from);
    v.Opacity(0);  // invisible until the fade's first tick
    if (pose.off.x != 0 || pose.off.y != 0) {
        auto slide = c.CreateSpringVector3Animation();
        slide.InitialValue(detail::Ref3(from));
        slide.FinalValue(detail::Ref3(rest));
        slide.DampingRatio(pose.damping);
        slide.Period(detail::Ms(static_cast<float>(kPeriodMs), speed));
        v.StartAnimation(L"Offset", slide);
    }
    if (pose.scale) {
        auto sz = v.Size();
        v.CenterPoint({sz.x * corner.x, sz.y * corner.y, 0});
        v.Scale({detail::kSwoopFrom, detail::kSwoopFrom, 1});
        auto grow = c.CreateVector3KeyFrameAnimation();
        grow.InsertKeyFrame(1.0f, {1, 1, 1}, c.CreateCubicBezierEasingFunction({0.0f, 0.0f}, {0.2f, 1.0f}));
        grow.Duration(detail::Ms(static_cast<float>(kFadeInMs), speed));
        v.StartAnimation(L"Scale", grow);
    }
    auto fade = c.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, 0.0f);
    fade.InsertKeyFrame(1.0f, 1.0f, c.CreateCubicBezierEasingFunction({0.0f, 0.0f}, {0.2f, 1.0f}));
    fade.Duration(detail::Ms(static_cast<float>(kFadeInMs), speed));
    v.StartAnimation(L"Opacity", fade);
}

inline void Out(detail::wuc::Compositor const& c, detail::wuc::Visual const& v, detail::float3 rest, detail::float2 travel,
                settings::Surface const& st = {}, detail::float2 corner = {1, 1}) {
    using namespace std::chrono;
    detail::Settle(v);
    if (st.effect == settings::Effect::None) {
        v.StopAnimation(L"Offset");
        v.StopAnimation(L"Opacity");
        v.Opacity(0);
        return;
    }
    const float speed = settings::Speed(st);
    const auto dur = detail::Ms(static_cast<float>(kOutMs), speed);
    const auto pose = detail::PoseOf(st, travel);
    auto down = c.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.9f, 0.5f});  // the dock's hide ease-in
    if (pose.off.x != 0 || pose.off.y != 0) {
        auto slide = c.CreateVector3KeyFrameAnimation();
        // No key at 0: a close during the opening spring carries on from wherever the panel is.
        slide.InsertKeyFrame(1.0f, {rest.x + pose.off.x, rest.y + pose.off.y, rest.z}, down);
        slide.Duration(dur);
        v.StartAnimation(L"Offset", slide);
    }
    if (pose.scale) {
        auto sz = v.Size();
        v.CenterPoint({sz.x * corner.x, sz.y * corner.y, 0});
        auto shrink = c.CreateVector3KeyFrameAnimation();
        shrink.InsertKeyFrame(1.0f, {detail::kSwoopFrom, detail::kSwoopFrom, 1}, down);
        shrink.Duration(dur);
        v.StartAnimation(L"Scale", shrink);
    }
    auto fade = c.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, 0.0f, down);
    fade.Duration(dur);
    v.StartAnimation(L"Opacity", fade);
}

}  // namespace halo::motion
