#define main genie_visual_harness_main
#include "genie_dcomp_test.cpp"
#undef main

int main() {
    int failures = 0;
    Geometry w{100, 100, 600, 400};
    for (Geometry icon : {Geometry{400, 700, 48, 48}, Geometry{400, -20, 48, 48}}) {
        for (int row = 0; row <= 48; ++row) {
            for (int col = 0; col <= 1; ++col) {
                double x, y, tx = (double)col, ty = row / 48.0;
                LampVertex(tx, ty, 0, w, icon, x, y);
                if (fabs(x - (w.x + tx*w.width)) > .01 || fabs(y - (w.y + ty*w.height)) > .01) ++failures;
                for (int step = 0; step <= 100; ++step) {
                    LampVertex(tx, ty, step / 100.0, w, icon, x, y);
                    if (!isfinite(x) || !isfinite(y)) ++failures;
                }
                LampVertex(tx, ty, 1, w, icon, x, y);
                double targetY = icon.y + (icon.y < w.y ? icon.height : 0);
                if (fabs(x - (icon.x + tx*icon.width)) > .01 || fabs(y - targetY) > .01) ++failures;
            }
        }
    }
    printf("lamp geometry failures: %d\n", failures);
    return failures ? 1 : 0;
}
