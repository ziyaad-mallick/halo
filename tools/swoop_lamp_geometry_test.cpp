#include "../src/lamp_geometry.h"
#include <cstdio>

int wmain() {
    int failures = 0;
    halo::LampRect w{100, 100, 600, 400};
    for (auto icon : {halo::LampRect{400, 700, 48, 48}, halo::LampRect{400, -20, 48, 48}}) {
        for (int row = 0; row <= 32; ++row) {
            for (int col = 0; col <= 1; ++col) {
                double tx = col, ty = row / 32.0;
                auto start = halo::LampVertex(tx, ty, 0, w, icon);
                if (std::abs(start.x - (w.x + tx*w.width)) > .01 || std::abs(start.y - (w.y + ty*w.height)) > .01) ++failures;
                for (int i = 0; i <= 100; ++i) {
                    auto point = halo::LampVertex(tx, ty, i / 100.0, w, icon);
                    if (!std::isfinite(point.x) || !std::isfinite(point.y)) ++failures;
                }
                auto end = halo::LampVertex(tx, ty, 1, w, icon);
                double targetY = icon.y + (icon.y < w.y ? icon.height : 0);
                if (std::abs(end.x - (icon.x + tx*icon.width)) > .01 || std::abs(end.y - targetY) > .01) ++failures;
            }
        }
    }
    printf("lamp geometry failures: %d\n", failures);
    return failures ? 1 : 0;
}
