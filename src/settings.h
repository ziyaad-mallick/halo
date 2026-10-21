// Halo's user settings: %LOCALAPPDATA%\Halo\settings.ini, written by Halo Settings (halo-settings.exe) and
// watched by halo.exe. The keys, types, defaults and ranges are the table in settings.cpp; docs/SETTINGS.md
// is the contract. Values are clamped and bad ones fall back to the default, so a hand-edited file never
// breaks the shell.
#pragma once
#include <windows.h>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace halo::settings {

enum class Type { Bool, Int, Color, Effect, Theme };
enum class Effect { Rise, Bounce, Swipe, Fade, Swoop, None };
enum class Theme { System, Dark, Light };

// One key of the schema. For Int: lo..hi. For Color: `def` may be empty (= the theme's colour).
struct Key {
    const char* section;
    const char* name;
    Type type;
    const char* def;
    int lo = 0, hi = 0;
};
const std::vector<Key>& Schema();

// The glass and motion every surface has.
struct Surface {
    Effect effect = Effect::Rise;
    int speed = 100, motion = 100, opacity = 66, blur = 40;
    bool hasTint = false;
    uint32_t tint = 0;  // 0xRRGGBB, when hasTint
};
struct Taskbar : Surface {
    int iconSize = 48, magnification = 150, hideDelay = 500;
    bool autohide = true, bounceOnLaunch = true, runningDots = true;
};
struct ControlCenter : Surface {
    bool hotCorner = true, closeOnLeave = true, showMedia = true, showSliders = true;
    int cornerWidth = 20, cornerDelay = 120;
};
struct Start : Surface {
    int recentApps = 6, recentFiles = 12;
    bool dimDesktop = false;
};
struct Settings {
    Theme theme = Theme::System;
    uint32_t accent = 0x14A394;
    bool reduceMotion = false;
    bool halo2 = false;  // HALO 2.0: Start hosts the control centre; the right edge is notifications + calendar
    Taskbar taskbar;
    ControlCenter cc;
    Start start;
};

// Raw values by "section.key" (only keys in the schema; values already validated).
using Values = std::map<std::string, std::string>;

std::wstring Dir();   // %LOCALAPPDATA%\Halo
std::wstring Path();  // ...\settings.ini

Values Read();                       // the file, validated; missing keys absent
Values Defaults();                   // every key at its default
Settings Parse(Values const& v);     // typed, defaults filled in
bool Write(Values const& v);         // atomic (tmp + MoveFileEx); writes every schema key
// Validates one value for "section.key"; returns the normalised value, or "" when it is not acceptable.
std::string Normalize(std::string const& sectionKey, std::string const& value);

// The settings Halo runs with. Reload() re-reads the file; true when anything changed.
const Settings& Get();
bool Reload();

// Watches Dir() on its own thread and posts `msg` to `hwnd` (debounced 150 ms) when settings.ini changes.
void Watch(HWND hwnd, UINT msg);

// Helpers for the surfaces.
inline float Speed(Surface const& s) { return s.speed / 100.0f; }    // duration ÷ this, period ÷ this
inline float Amount(Surface const& s) { return s.motion / 100.0f; }  // travel × this

}  // namespace halo::settings
