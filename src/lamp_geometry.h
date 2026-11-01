#pragma once

#include <algorithm>
#include <cmath>

namespace halo {
// Lamp curve adapted from MacOS Minimize Animation by Abdullah Masood and
// Potassiumuncher (MIT); see mods/halo-genie.wh.cpp for the licence notice.
struct LampRect { double x, y, width, height; };
struct LampPoint { double x, y; };

// One timeline for every renderer. `p` is linear in time: the stretch (k) and
// collapse (j) phases below each carry their own ease, so easing time as well
// compounds into a dead start, a burst, and a long sliver at the end.
inline constexpr double kLampSeconds = .48;
inline constexpr double kStretchEnd = .5;     // k reaches 1 here
inline constexpr double kCollapseStart = .06; // j leaves 0 here
inline constexpr double kFadeStart = .6, kFadeEnd = .92;

inline double LampSmooth(double x) { x = std::clamp(x, 0.0, 1.0); return x*x*(3-2*x); }
inline double LampOpacity(double p) { return 1 - LampSmooth((p - kFadeStart) / (kFadeEnd - kFadeStart)); }

inline LampPoint LampVertex(double tx, double ty, double p, LampRect w, LampRect icon) {
    const bool mirror = icon.y + icon.height / 2 < w.y + w.height / 2;
    if (mirror) { w.y = -w.y - w.height; icon.y = -icon.y - icon.height; ty = 1 - ty; }
    // Stretch toward the icon, then collapse into it; the phases overlap and
    // both have zero slope at their endpoints, so there is no velocity jump.
    const double k = LampSmooth(p / kStretchEnd), j = LampSmooth((p - kCollapseStart) / (1 - kCollapseStart));
    const double extra = icon.y - w.y - w.height;
    const double full = icon.y - w.y - extra * (1 - k);
    const double height = full * (1 - j), y = ty * height;
    const double denom = std::abs(full) < .001 ? (full < 0 ? -.001 : .001) : full;
    constexpr double pi = 3.14159265358979323846;
    LampPoint out{
        w.x + tx * icon.width + tx * (w.width - icon.width) * (1 - j) * (1 - ty)
            + tx * (w.width - icon.width) * (1 - k) * ty
            + (icon.x - w.x) * (y / (full + (full < 0 ? -.1 : .1))) * k + (icon.x - w.x) * j
            + std::sin((height - y) / denom * 2 * pi + pi) * (w.x + w.width * tx - icon.x - icon.width * tx) / 7 * k,
        w.y + y + icon.y - w.y - height - extra * (1 - k)};
    if (mirror) out.y = -out.y;
    return out;
}

// An affine strip follows the centre line at both edges while using the mean
// width of those edges. This reduces stair-stepping without per-frame CPU work.
struct LampAffine { double sx, shear, tx, sy, ty; };
inline LampAffine LampStripAffine(double top, double bottom, double p, LampRect w, LampRect icon,
                                 double sourceWidth, double sourceHeight) {
    auto l0 = LampVertex(0, top, p, w, icon), r0 = LampVertex(1, top, p, w, icon);
    auto l1 = LampVertex(0, bottom, p, w, icon), r1 = LampVertex(1, bottom, p, w, icon);
    double srcTop = sourceHeight * top, srcH = sourceHeight * (bottom-top);
    double width = ((r0.x-l0.x)+(r1.x-l1.x))/2;
    double center0 = (l0.x+r0.x)/2, center1 = (l1.x+r1.x)/2;
    double shear = (center1-center0)/srcH, sy = (l1.y-l0.y)/srcH;
    return {width/sourceWidth, shear, center0-width/2-w.x-shear*srcTop,
            sy, l0.y-w.y-sy*srcTop};
}
}  // namespace halo
