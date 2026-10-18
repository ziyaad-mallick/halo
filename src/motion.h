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
// Give the window kPad logical px of empty margin around the visual so the overshoot is never clipped.
#pragma once
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <chrono>

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
}  // namespace detail

inline void In(detail::wuc::Compositor const& c, detail::wuc::Visual const& v, detail::float3 rest, detail::float2 travel) {
    using namespace std::chrono;
    detail::Settle(v);
    v.Offset({rest.x + travel.x, rest.y + travel.y, rest.z});
    v.Opacity(0);  // invisible until the fade's first tick
    auto slide = c.CreateSpringVector3Animation();
    slide.InitialValue(detail::Ref3({rest.x + travel.x, rest.y + travel.y, rest.z}));
    slide.FinalValue(detail::Ref3(rest));
    slide.DampingRatio(kDamping);
    slide.Period(milliseconds(kPeriodMs));
    v.StartAnimation(L"Offset", slide);
    auto fade = c.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(0.0f, 0.0f);
    fade.InsertKeyFrame(1.0f, 1.0f, c.CreateCubicBezierEasingFunction({0.0f, 0.0f}, {0.2f, 1.0f}));
    fade.Duration(milliseconds(kFadeInMs));
    v.StartAnimation(L"Opacity", fade);
}

inline void Out(detail::wuc::Compositor const& c, detail::wuc::Visual const& v, detail::float3 rest, detail::float2 travel) {
    using namespace std::chrono;
    detail::Settle(v);
    auto down = c.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.9f, 0.5f});  // the dock's hide ease-in
    auto slide = c.CreateVector3KeyFrameAnimation();
    // No key at 0: a close during the opening spring carries on from wherever the panel is.
    slide.InsertKeyFrame(1.0f, {rest.x + travel.x, rest.y + travel.y, rest.z}, down);
    slide.Duration(milliseconds(kOutMs));
    v.StartAnimation(L"Offset", slide);
    auto fade = c.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, 0.0f, down);
    fade.Duration(milliseconds(kOutMs));
    v.StartAnimation(L"Opacity", fade);
}

}  // namespace halo::motion
