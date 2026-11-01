#include "../src/lamp_geometry.h"
#include <cstdio>
#include <vector>

static double Sample(double u, double tx, double ty, halo::LampRect w, halo::LampRect icon, int axis, bool reverse) {
    double t = reverse ? 1-u : u;
    auto p = halo::LampVertex(tx, ty, t, w, icon);
    return axis ? p.y : p.x;
}

int wmain() {
    int failures = 0;
    halo::LampRect w{100, 100, 600, 400};
    for (auto icon : {halo::LampRect{400, 700, 48, 48}, halo::LampRect{400, -20, 48, 48}}) {
        for (int row = 0; row <= 64; ++row) {
            for (int col = 0; col <= 1; ++col) {
                double tx = col, ty = row / 64.0;
                auto start = halo::LampVertex(tx, ty, 0, w, icon);
                if (std::abs(start.x - (w.x + tx*w.width)) > .01 || std::abs(start.y - (w.y + ty*w.height)) > .01) ++failures;
                for (int i = 0; i <= 100; ++i) {
                    auto point = halo::LampVertex(tx, ty, i / 100.0, w, icon);
                    if (!std::isfinite(point.x) || !std::isfinite(point.y)) ++failures;
                }
                auto end = halo::LampVertex(tx, ty, 1, w, icon);
                double targetY = icon.y + (icon.y < w.y ? icon.height : 0);
                if (std::abs(end.x - (icon.x + tx*icon.width)) > .01 || std::abs(end.y - targetY) > .01) ++failures;
                for (double boundary : {halo::kCollapseStart, halo::kStretchEnd}) {
                    const double e = .0001;
                    auto a = halo::LampVertex(tx, ty, boundary-e, w, icon);
                    auto b = halo::LampVertex(tx, ty, boundary, w, icon);
                    auto c = halo::LampVertex(tx, ty, boundary+e, w, icon);
                    // A velocity discontinuity produces a visible jerk in the lamp motion.
                    if (std::abs((b.x-a.x) - (c.x-b.x)) > .002 || std::abs((b.y-a.y) - (c.y-b.y)) > .002) ++failures;
                }
            }
        }
    }
    std::vector<double> knots;
    for (int i = 0; i <= 16; ++i) knots.push_back(i/16.0);
    for (double v : {halo::kCollapseStart, halo::kStretchEnd, halo::kFadeStart, halo::kFadeEnd}) { knots.push_back(v); knots.push_back(1-v); }
    std::sort(knots.begin(), knots.end());
    knots.erase(std::unique(knots.begin(), knots.end(), [](double x, double y) { return y-x < 1e-9; }), knots.end());
    double maxError = 0;
    for (auto pair : {std::pair{halo::LampRect{0,0,1920,1080},halo::LampRect{1400,1040,64,64}},
                      std::pair{halo::LampRect{0,0,600,400},halo::LampRect{1200,900,48,48}},
                      std::pair{halo::LampRect{0,0,1920,1080},halo::LampRect{-1000,1400,64,64}}}) {
        for (int row = 0; row <= 64; row += 4) for (int x = 0; x <= 1; ++x)
            for (int axis = 0; axis < 2; ++axis) for (bool reverse : {false, true}) {
                auto f = [&](double u) { return Sample(u,x,row/64.0,pair.first,pair.second,axis,reverse); };
                auto d = [&](double u) { double a=std::max(0.0,u-.0001), b=std::min(1.0,u+.0001); return (f(b)-f(a))/(b-a); };
                for (size_t i = 0; i+1 < knots.size(); ++i) {
                    double a=knots[i], b=knots[i+1], h=b-a, y0=f(a), y1=f(b), m0=d(a)*h, m1=d(b)*h;
                    for (int s = 1; s < 10; ++s) {
                        double z=s/10.0, z2=z*z, z3=z2*z;
                        double estimate=(2*z3-3*z2+1)*y0+(z3-2*z2+z)*m0+(-2*z3+3*z2)*y1+(z3-z2)*m1;
                        maxError=std::max(maxError,std::abs(estimate-f(a+z*h)));
                    }
                }
            }
    }
    printf("maximum sampled compositor curve error: %.3f px\n", maxError);
    if (maxError > 2.0) ++failures;
    for (auto pair : {std::pair{halo::LampRect{0,0,1920,1080},halo::LampRect{1400,1040,64,64}},
                      std::pair{halo::LampRect{0,0,600,400},halo::LampRect{1200,900,48,48}},
                      std::pair{halo::LampRect{0,0,1920,1080},halo::LampRect{-1000,1400,64,64}}}) {
        auto error = [&](int rows, bool affine) {
            double worst = 0;
            for (int row=0; row<rows; ++row) for (int step=0; step<=100; ++step) {
                double p=step/100.0, top=row/(double)rows, bottom=(row+1)/(double)rows;
                auto m=halo::LampStripAffine(top,bottom,p,pair.first,pair.second,pair.first.width,pair.first.height);
                for (int x=0; x<=1; ++x) for (int fraction=0; fraction<=4; ++fraction) {
                    double ty=(row+fraction/4.0)/rows, srcY=ty*pair.first.height;
                    auto actual=halo::LampVertex(x,ty,p,pair.first,pair.second);
                    auto old=halo::LampVertex(x,top,p,pair.first,pair.second);
                    double predicted=affine ? m.sx*x*pair.first.width+m.shear*srcY+m.tx+pair.first.x : old.x;
                    worst=std::max(worst,std::abs(actual.x-predicted));
                    if (affine && std::abs(actual.y-(m.sy*srcY+m.ty+pair.first.y)) > .001) ++failures;
                }
            }
            return worst;
        };
        double baseline=error(32,false), improved=error(48,true);
        printf("sampled edge deviation: old=%.2f px affine=%.2f px\n", baseline, improved);
        if (!(improved < baseline*.25)) ++failures;
    }
    printf("lamp geometry failures: %d\n", failures);
    return failures ? 1 : 0;
}
