// Shared colour tokens for every Halo surface (dock, cluster, labels,
// previews, launcher, Control Center, notifications).
//
// Follows the Windows app mode (Settings > Personalization > Colors): light is
// frosted white glass, dark ink, a hairline outer edge and a soft shadow; dark
// is the original #16161A@50% liquid glass. HALO_THEME=light|dark overrides, then settings.ini's
// [general] theme and accent. Refresh() re-reads them after settings::Reload(); P() always returns the
// same object, so a `const auto& pal = P()` kept across a Refresh() sees the new colours.
#pragma once
#include <windows.h>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <d2d1.h>
#include <winrt/Windows.UI.h>
#include "settings.h"

namespace halo::theme {

struct Argb { uint8_t a, r, g, b; };

struct Palette {
    // Glass: blur radius over the backdrop, then a tint layer, then a noise film.
    float blur;
    Argb tint;          // the frost layer over the blurred backdrop
    float noise;        // noise opacity
    Argb rimTop, rimMid, rimLow, rimBottom;  // inner rim gradient stops (0, .35, .7, 1)
    Argb edge;          // 1px outer hairline that separates glass from a light wallpaper
    Argb shadow;        // drop shadow colour (alpha = strength)
    float shadowBlur, shadowY;

    // Ink, as D2D float RGBA.
    D2D1_COLOR_F text, textSoft, dim, faint;
    D2D1_COLOR_F accent, onAccent;
    D2D1_COLOR_F fill;        // resting tile / chip fill
    D2D1_COLOR_F fillHover;   // hovered tile
    D2D1_COLOR_F fillPress;
    D2D1_COLOR_F line;        // hairline separators inside panels
    D2D1_COLOR_F off;         // inactive round toggle
    Argb divider;             // dock dividers
    Argb dot;                 // running-app dot
    Argb veil;                // full-screen dim behind the launcher
    D2D1_COLOR_F labelBg, labelRim, labelText;
};

// The glass a surface builds: the palette's tint scaled by the surface's opacity (66 % = as designed, so a
// light theme keeps its own stronger frost) with the surface's tint colour, if it has one.
// `blur` is carried for completeness: the backdrop is DWM's host backdrop, whose blur radius is not ours to set.
struct GlassStyle {
    Argb tint;
    float blur;
};

namespace detail {
inline bool g_have = false, g_dark = true;
inline Palette& Cur() { static Palette p{}; return p; }  // constant-initialised: safe to reference at static init

inline const Palette& Base(bool dark) {
    static const Palette light{
        40.0f, {0xD6, 0xF8, 0xF8, 0xFB}, 0.018f,
        {0xCC, 0xFF, 0xFF, 0xFF}, {0x40, 0xFF, 0xFF, 0xFF}, {0x18, 0xFF, 0xFF, 0xFF}, {0x50, 0xFF, 0xFF, 0xFF},
        {0x1C, 0x00, 0x00, 0x00}, {0x2E, 0x00, 0x00, 0x00}, 28.0f, 8.0f,
        {0.11f, 0.11f, 0.12f, 0.92f}, {0.11f, 0.11f, 0.12f, 0.80f}, {0.11f, 0.11f, 0.12f, 0.52f}, {0.11f, 0.11f, 0.12f, 0.26f},
        {0x12 / 255.0f, 0x85 / 255.0f, 0x7B / 255.0f, 1.0f}, {1, 1, 1, 1},  // accent: dark teal #12857B (#0F766E read as off on dark glass)
        {1, 1, 1, 0.72f}, {1, 1, 1, 0.90f}, {0, 0, 0, 0.06f},
        {0, 0, 0, 0.08f},
        {0, 0, 0, 0.07f},
        {0x24, 0x00, 0x00, 0x00}, {0xB0, 0x1D, 0x1D, 0x1F}, {0x38, 0xF2, 0xF2, 0xF5},
        {1, 1, 1, 0.86f}, {0, 0, 0, 0.10f}, {0.11f, 0.11f, 0.12f, 0.92f},
    };
    static const Palette darkP{
        40.0f, {0xA8, 0x18, 0x19, 0x1E}, 0.025f,  // smoked glass: #18191E at 66%
        {0x70, 0xFF, 0xFF, 0xFF}, {0x18, 0xFF, 0xFF, 0xFF}, {0x0A, 0xFF, 0xFF, 0xFF}, {0x26, 0xFF, 0xFF, 0xFF},
        {0x00, 0x00, 0x00, 0x00}, {0x55, 0x00, 0x00, 0x00}, 28.0f, 10.0f,  // a shadow in dark too, or panels melt into bright windows
        {1, 1, 1, 0.96f}, {1, 1, 1, 0.88f}, {1, 1, 1, 0.70f}, {1, 1, 1, 0.30f},
        {0x14 / 255.0f, 0xA3 / 255.0f, 0x94 / 255.0f, 1.0f}, {1, 1, 1, 1},  // teal #14A394: #12857B went muddy on dark glass
        {1, 1, 1, 0.10f}, {1, 1, 1, 0.16f}, {1, 1, 1, 0.06f},
        {1, 1, 1, 0.12f},
        {1, 1, 1, 0.16f},
        {0x33, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF}, {0xB0, 0x16, 0x16, 0x1A},
        {0x16 / 255.0f, 0x16 / 255.0f, 0x1A / 255.0f, 0.88f}, {1, 1, 1, 0.18f}, {1, 1, 1, 0.95f},
    };
    return dark ? darkP : light;
}

inline bool SystemDark() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}
}  // namespace detail

// Re-reads the theme override and accent from settings::Get() (call after settings::Reload()).
inline void Refresh() {
    auto const& cfg = halo::settings::Get();
    wchar_t v[16] = {};
    bool dark;
    if (GetEnvironmentVariableW(L"HALO_THEME", v, 16)) dark = _wcsicmp(v, L"dark") == 0;  // tests
    else if (cfg.theme != halo::settings::Theme::System) dark = cfg.theme == halo::settings::Theme::Dark;
    else dark = detail::SystemDark();
    Palette p = detail::Base(dark);
    if (cfg.accent != 0x14A394)  // the default keeps each theme's own teal
        p.accent = {((cfg.accent >> 16) & 0xFF) / 255.0f, ((cfg.accent >> 8) & 0xFF) / 255.0f, (cfg.accent & 0xFF) / 255.0f, 1.0f};
    detail::Cur() = p;
    detail::g_dark = dark;
    detail::g_have = true;
}

inline bool Dark() {
    if (!detail::g_have) Refresh();
    return detail::g_dark;
}

inline const Palette& P() {
    if (!detail::g_have) Refresh();
    return detail::Cur();
}

inline GlassStyle Glass(halo::settings::Surface const& g) {
    auto const& p = P();
    Argb t = p.tint;
    t.a = static_cast<uint8_t>(std::min(255L, std::lround(p.tint.a * g.opacity / 66.0)));
    if (g.hasTint) { t.r = (g.tint >> 16) & 0xFF; t.g = (g.tint >> 8) & 0xFF; t.b = g.tint & 0xFF; }
    return {t, static_cast<float>(g.blur)};
}

inline winrt::Windows::UI::Color C(Argb c) { return winrt::Windows::UI::ColorHelper::FromArgb(c.a, c.r, c.g, c.b); }
inline D2D1_COLOR_F A(D2D1_COLOR_F c, float alpha) { c.a *= alpha; return c; }

}  // namespace halo::theme
