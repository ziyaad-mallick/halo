// Shared colour tokens for every Halo surface (dock, cluster, labels,
// previews, launcher, Control Center, notifications).
//
// Follows the Windows app mode (Settings > Personalization > Colors): light is
// frosted white glass, dark ink, a hairline outer edge and a soft shadow; dark
// is the original #16161A@50% liquid glass. HALO_THEME=light|dark overrides.
// Read once per process.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdlib>
#include <d2d1.h>
#include <winrt/Windows.UI.h>

namespace halo::theme {

struct Argb { uint8_t a, r, g, b; };

inline bool Dark() {
    static const bool dark = [] {
        wchar_t v[16] = {};
        if (GetEnvironmentVariableW(L"HALO_THEME", v, 16)) return _wcsicmp(v, L"dark") == 0;
        DWORD light = 1, size = sizeof(light);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
        return light == 0;
    }();
    return dark;
}

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

inline const Palette& P() {
    static const Palette light{
        40.0f, {0xD6, 0xF8, 0xF8, 0xFB}, 0.018f,
        {0xCC, 0xFF, 0xFF, 0xFF}, {0x40, 0xFF, 0xFF, 0xFF}, {0x18, 0xFF, 0xFF, 0xFF}, {0x50, 0xFF, 0xFF, 0xFF},
        {0x1C, 0x00, 0x00, 0x00}, {0x2E, 0x00, 0x00, 0x00}, 28.0f, 8.0f,
        {0.11f, 0.11f, 0.12f, 0.92f}, {0.11f, 0.11f, 0.12f, 0.80f}, {0.11f, 0.11f, 0.12f, 0.52f}, {0.11f, 0.11f, 0.12f, 0.26f},
        {0x0F / 255.0f, 0x76 / 255.0f, 0x6E / 255.0f, 1.0f}, {1, 1, 1, 1},  // accent: dark teal #0F766E
        {1, 1, 1, 0.72f}, {1, 1, 1, 0.90f}, {0, 0, 0, 0.06f},
        {0, 0, 0, 0.08f},
        {0, 0, 0, 0.07f},
        {0x24, 0x00, 0x00, 0x00}, {0xB0, 0x1D, 0x1D, 0x1F}, {0x38, 0xF2, 0xF2, 0xF5},
        {1, 1, 1, 0.86f}, {0, 0, 0, 0.10f}, {0.11f, 0.11f, 0.12f, 0.92f},
    };
    static const Palette dark{
        40.0f, {0xB8, 0x10, 0x10, 0x13}, 0.025f,  // smoked glass: #101013 at 72%
        {0x70, 0xFF, 0xFF, 0xFF}, {0x18, 0xFF, 0xFF, 0xFF}, {0x0A, 0xFF, 0xFF, 0xFF}, {0x26, 0xFF, 0xFF, 0xFF},
        {0x00, 0x00, 0x00, 0x00}, {0x55, 0x00, 0x00, 0x00}, 28.0f, 10.0f,  // a shadow in dark too, or panels melt into bright windows
        {1, 1, 1, 0.95f}, {1, 1, 1, 0.85f}, {1, 1, 1, 0.58f}, {1, 1, 1, 0.26f},
        {0x0F / 255.0f, 0x76 / 255.0f, 0x6E / 255.0f, 1.0f}, {1, 1, 1, 1},
        {1, 1, 1, 0.12f}, {1, 1, 1, 0.20f}, {1, 1, 1, 0.07f},
        {1, 1, 1, 0.12f},
        {1, 1, 1, 0.17f},
        {0x33, 0xFF, 0xFF, 0xFF}, {0xFF, 0xFF, 0xFF, 0xFF}, {0xB0, 0x16, 0x16, 0x1A},
        {0x16 / 255.0f, 0x16 / 255.0f, 0x1A / 255.0f, 0.88f}, {1, 1, 1, 0.18f}, {1, 1, 1, 0.95f},
    };
    return Dark() ? dark : light;
}

inline winrt::Windows::UI::Color C(Argb c) { return winrt::Windows::UI::ColorHelper::FromArgb(c.a, c.r, c.g, c.b); }
inline D2D1_COLOR_F A(D2D1_COLOR_F c, float alpha) { c.a *= alpha; return c; }

}  // namespace halo::theme
