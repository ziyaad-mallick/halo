#pragma once

#include <algorithm>
#include <cmath>

namespace halo {
// Lamp curve adapted from MacOS Minimize Animation by Abdullah Masood and
// Potassiumuncher (MIT); see mods/halo-genie.wh.cpp for the licence notice.
struct LampRect { double x, y, width, height; };
struct LampPoint { double x, y; };

inline LampPoint LampVertex(double tx, double ty, double p, LampRect w, LampRect icon) {
    const bool mirror = icon.y + icon.height / 2 < w.y + w.height / 2;
    if (mirror) { w.y = -w.y - w.height; icon.y = -icon.y - icon.height; ty = 1 - ty; }
    // Blend the stretch and collapse phases, rather than switching abruptly
    // from one to the other at p=.3. Both have zero slope at their endpoints.
    auto smooth = [](double x) { x = std::clamp(x, 0.0, 1.0); return x*x*(3-2*x); };
    const double k = smooth(p / .55), j = smooth((p - .12) / .88);
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
}  // namespace halo
