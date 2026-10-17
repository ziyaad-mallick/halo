// Shared open/close motion for Halo's panels (Control Center, Notification Center, launcher), so
// every surface comes and goes with the dock's own spring. All of it runs on the compositor: one
// StartAnimation per property, no timers, no per-frame CPU.
//
// In:  the panel starts `travel` away from its rest offset (e.g. {0, -28*s} drops it from above,
//      {+40*s, 0} slides it in from the right), springs home with the dock's reveal spring (slight
//      overshoot), scales up from 0.92 around `anchor` and fades in over the first 110 ms.
// Out: a small anticipation (it nudges 8% of `travel` the other way and swells to 1.015), then
//      accelerates back out along `travel`, shrinks to 0.94 and fades. kOutMs long; destroy or hide
//      the window after it (a timer of kOutMs + 10 is fine).
//
// The visual's resting Offset must be `rest`. Give the window kPad logical px of empty margin on
// every side of the visual so the overshoot and the anticipation are never clipped.
#pragma once
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <chrono>

namespace halo::motion {

constexpr float kDamping = 0.72f;  // the dock's reveal spring (halo.cpp kSpringDamping)
constexpr int kPeriodMs = 55;      // halo.cpp kSpringPeriodMs
constexpr unsigned kOutMs = 200;
constexpr float kPad = 16.0f;  // logical px of margin a panel window keeps around its visual

namespace detail {
namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
}  // namespace detail

inline void In(detail::wuc::Compositor const& c, detail::wuc::Visual const& v, detail::float3 rest,
               detail::float2 anchor, detail::float2 travel) {
    using namespace std::chrono;
    v.CenterPoint({anchor.x, anchor.y, 0});
    auto slide = c.CreateSpringVector3Animation();
    slide.InitialValue({rest.x + travel.x, rest.y + travel.y, rest.z});
    slide.FinalValue(rest);
    slide.DampingRatio(kDamping);
    slide.Period(milliseconds(kPeriodMs));
    v.StartAnimation(L"Offset", slide);
    auto scale = c.CreateSpringVector3Animation();
    scale.InitialValue({0.92f, 0.92f, 1.0f});
    scale.FinalValue({1.0f, 1.0f, 1.0f});
    scale.DampingRatio(kDamping);
    scale.Period(milliseconds(kPeriodMs));
    v.StartAnimation(L"Scale", scale);
    auto fade = c.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, 0.0f);
    fade.InsertKeyFrame(1.0f, 1.0f, c.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f}));
    fade.Duration(milliseconds(110));
    v.StartAnimation(L"Opacity", fade);
}

inline void Out(detail::wuc::Compositor const& c, detail::wuc::Visual const& v, detail::float3 rest,
                detail::float2 anchor, detail::float2 travel) {
    using namespace std::chrono;
    v.CenterPoint({anchor.x, anchor.y, 0});
    auto up = c.CreateCubicBezierEasingFunction({0.3f, 0.5f}, {0.5f, 1.0f});    // dock.cpp BounceUp
    auto down = c.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.9f, 0.5f});  // the dock's hide ease-in
    auto slide = c.CreateVector3KeyFrameAnimation();
    slide.InsertKeyFrame(0.0f, rest);
    slide.InsertKeyFrame(0.3f, {rest.x - travel.x * 0.08f, rest.y - travel.y * 0.08f, rest.z}, up);
    slide.InsertKeyFrame(1.0f, {rest.x + travel.x, rest.y + travel.y, rest.z}, down);
    slide.Duration(milliseconds(kOutMs));
    v.StartAnimation(L"Offset", slide);
    auto scale = c.CreateVector3KeyFrameAnimation();
    scale.InsertKeyFrame(0.0f, {1.0f, 1.0f, 1.0f});
    scale.InsertKeyFrame(0.3f, {1.015f, 1.015f, 1.0f}, up);
    scale.InsertKeyFrame(1.0f, {0.94f, 0.94f, 1.0f}, down);
    scale.Duration(milliseconds(kOutMs));
    v.StartAnimation(L"Scale", scale);
    auto fade = c.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, 1.0f);
    fade.InsertKeyFrame(0.35f, 1.0f);
    fade.InsertKeyFrame(1.0f, 0.0f, down);
    fade.Duration(milliseconds(kOutMs));
    v.StartAnimation(L"Opacity", fade);
}

}  // namespace halo::motion
