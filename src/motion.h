// Shared open/close motion for Halo's panels (Control Center, Notification Center, launcher): the dock's
// own reveal, a slide and nothing else. All of it runs on the compositor: one StartAnimation, no timers,
// no per-frame CPU.
//
// Slide only, on purpose. These panels are ~15x the dock's area and sit on a live backdrop; animating
// their Opacity makes DWM render the whole panel into an offscreen layer every frame, and animating
// Scale resamples it every frame, and together they dropped frames where the dock's offset-only
// reveal did not (measured with tools/framegaps.cpp). So a panel travels in from beyond its window's
// edge, which clips it: pass a `travel` of at least the panel's size plus its margin, toward the
// screen edge it should come out of.
//
// In:  springs from rest + travel to rest with the dock's reveal spring (slight overshoot).
// Out: a small anticipation (it nudges up to 8 px the other way), then accelerates back out along
//      `travel` on the dock's hide ease. kOutMs long; destroy or hide the window after it (a timer of
//      kOutMs + 10 is fine).
//
// Give the window kPad logical px of empty margin around the visual so the overshoot is never clipped.
#pragma once
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace halo::motion {

constexpr float kDamping = 0.72f;  // the dock's reveal spring (halo.cpp kSpringDamping)
constexpr int kPeriodMs = 55;      // halo.cpp kSpringPeriodMs
constexpr unsigned kOutMs = 200;
constexpr float kPad = 16.0f;  // logical px of margin a panel window keeps around its visual

namespace detail {
namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;
inline winrt::Windows::Foundation::IReference<float3> Ref3(float3 v) { return v; }  // spring animations take IReference<float3>
// A panel reopened mid-close may still carry the old Scale/Opacity: put both back at rest.
inline void Settle(wuc::Visual const& v) {
    v.StopAnimation(L"Scale");
    v.StopAnimation(L"Opacity");
    v.Scale({1, 1, 1});
    v.Opacity(1);
}
}  // namespace detail

inline void In(detail::wuc::Compositor const& c, detail::wuc::Visual const& v, detail::float3 rest, detail::float2 travel) {
    using namespace std::chrono;
    detail::Settle(v);
    auto slide = c.CreateSpringVector3Animation();
    slide.InitialValue(detail::Ref3({rest.x + travel.x, rest.y + travel.y, rest.z}));
    slide.FinalValue(detail::Ref3(rest));
    slide.DampingRatio(kDamping);
    slide.Period(milliseconds(kPeriodMs));
    v.StartAnimation(L"Offset", slide);
}

inline void Out(detail::wuc::Compositor const& c, detail::wuc::Visual const& v, detail::float3 rest, detail::float2 travel) {
    using namespace std::chrono;
    detail::Settle(v);
    auto up = c.CreateCubicBezierEasingFunction({0.3f, 0.5f}, {0.5f, 1.0f});    // dock.cpp BounceUp
    auto down = c.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.9f, 0.5f});  // the dock's hide ease-in
    const float nudge = std::min(0.04f, 8.0f / std::max(1.0f, std::hypot(travel.x, travel.y)));  // at most 8 px back
    auto slide = c.CreateVector3KeyFrameAnimation();
    slide.InsertKeyFrame(0.0f, rest);
    slide.InsertKeyFrame(0.25f, {rest.x - travel.x * nudge, rest.y - travel.y * nudge, rest.z}, up);
    slide.InsertKeyFrame(1.0f, {rest.x + travel.x, rest.y + travel.y, rest.z}, down);
    slide.Duration(milliseconds(kOutMs));
    v.StartAnimation(L"Offset", slide);
}

}  // namespace halo::motion
