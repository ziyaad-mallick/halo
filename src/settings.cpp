#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace halo::settings {

const std::vector<Key>& Schema() {
    static const std::vector<Key> keys = {
        {"general", "theme", Type::Theme, "system"},
        {"general", "accent", Type::Color, "#14A394"},
        {"general", "reduce_motion", Type::Bool, "off"},

        {"taskbar", "effect", Type::Effect, "rise"},
        {"taskbar", "speed", Type::Int, "100", 50, 200},
        {"taskbar", "motion", Type::Int, "100", 0, 200},
        {"taskbar", "opacity", Type::Int, "66", 20, 100},
        {"taskbar", "tint", Type::Color, ""},
        {"taskbar", "blur", Type::Int, "40", 0, 80},
        {"taskbar", "icon_size", Type::Int, "48", 32, 72},
        {"taskbar", "magnification", Type::Int, "150", 100, 250},
        {"taskbar", "autohide", Type::Bool, "on"},
        {"taskbar", "hide_delay", Type::Int, "500", 0, 2000},
        {"taskbar", "bounce_on_launch", Type::Bool, "on"},
        {"taskbar", "running_dots", Type::Bool, "on"},

        {"controlcenter", "effect", Type::Effect, "rise"},
        {"controlcenter", "speed", Type::Int, "100", 50, 200},
        {"controlcenter", "motion", Type::Int, "100", 0, 200},
        {"controlcenter", "opacity", Type::Int, "66", 20, 100},
        {"controlcenter", "tint", Type::Color, ""},
        {"controlcenter", "blur", Type::Int, "40", 0, 80},
        {"controlcenter", "hot_corner", Type::Bool, "on"},
        {"controlcenter", "corner_width", Type::Int, "20", 5, 50},
        {"controlcenter", "corner_delay", Type::Int, "120", 0, 600},
        {"controlcenter", "close_on_leave", Type::Bool, "on"},
        {"controlcenter", "show_media", Type::Bool, "on"},
        {"controlcenter", "show_sliders", Type::Bool, "on"},

        {"start", "effect", Type::Effect, "rise"},
        {"start", "speed", Type::Int, "100", 50, 200},
        {"start", "motion", Type::Int, "100", 0, 200},
        {"start", "opacity", Type::Int, "66", 20, 100},
        {"start", "tint", Type::Color, ""},
        {"start", "blur", Type::Int, "40", 0, 80},
        {"start", "recent_apps", Type::Int, "6", 0, 6},
        {"start", "recent_files", Type::Int, "12", 0, 12},
        {"start", "dim_desktop", Type::Bool, "off"},
    };
    return keys;
}

namespace {

constexpr const char* kEffects[] = {"rise", "bounce", "swipe", "fade", "swoop", "none"};
constexpr const char* kThemes[] = {"system", "dark", "light"};

std::string Trim(std::string s) {
    auto sp = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && sp(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && sp(s[i])) i++;
    return s.substr(i);
}
std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
Key const* Find(std::string const& sectionKey) {
    for (auto const& k : Schema())
        if (sectionKey == std::string(k.section) + "." + k.name) return &k;
    return nullptr;
}
std::string Get(Values const& v, Key const& k) {
    auto it = v.find(std::string(k.section) + "." + k.name);
    return it != v.end() ? it->second : k.def;
}
uint32_t Hex(std::string const& c) { return static_cast<uint32_t>(strtoul(c.c_str() + 1, nullptr, 16)); }

Settings g_current;
Values g_values;
bool g_loaded = false;

}  // namespace

std::string Normalize(std::string const& sectionKey, std::string const& value) {
    Key const* k = Find(sectionKey);
    if (!k) return {};
    std::string v = Lower(Trim(value));
    switch (k->type) {
        case Type::Bool:
            if (v == "on" || v == "true" || v == "1" || v == "yes") return "on";
            if (v == "off" || v == "false" || v == "0" || v == "no") return "off";
            return {};
        case Type::Int: {
            if (v.empty()) return {};
            char* end = nullptr;
            long n = strtol(v.c_str(), &end, 10);
            if (!end || *end) return {};
            return std::to_string(std::clamp<long>(n, k->lo, k->hi));
        }
        case Type::Color:
            if (v.empty() && !*k->def) return "";  // "" is a valid tint: the theme's own
            if (v.size() == 7 && v[0] == '#' && v.find_first_not_of("0123456789abcdef", 1) == std::string::npos) {
                for (auto& c : v) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
                return v;
            }
            return {};
        case Type::Effect:
            for (auto e : kEffects) if (v == e) return v;
            return {};
        case Type::Theme:
            for (auto t : kThemes) if (v == t) return v;
            return {};
    }
    return {};
}

std::wstring Dir() {
    wchar_t buf[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH)) return {};
    return std::wstring(buf) + L"\\Halo";
}
std::wstring Path() {
    wchar_t over[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"HALO_SETTINGS_FILE", over, MAX_PATH)) return over;  // tests
    return Dir() + L"\\settings.ini";
}

Values Read() {
    Values out;
    FILE* f = _wfopen(Path().c_str(), L"rb");
    if (!f) return out;
    std::string text;
    char buf[4096];
    for (size_t n; text.size() < (1 << 20) && (n = fread(buf, 1, sizeof buf, f)) > 0;) text.append(buf, n);  // ~1 KB in practice; a runaway file is not read whole
    fclose(f);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    std::string section;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = Trim(text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') { section = Lower(Trim(line.substr(1, line.size() - 2))); continue; }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string sk = section + "." + Lower(Trim(line.substr(0, eq)));
        std::string raw = Trim(line.substr(eq + 1));
        if (size_t sc = raw.find(" ;"); sc != std::string::npos) raw = Trim(raw.substr(0, sc));
        if (!Find(sk)) continue;
        std::string v = Normalize(sk, raw);
        if (!v.empty() || (Find(sk)->type == Type::Color && raw.empty())) out[sk] = v;
    }
    return out;
}

Values Defaults() {
    Values v;
    for (auto const& k : Schema()) v[std::string(k.section) + "." + k.name] = k.def;
    return v;
}

Settings Parse(Values const& v) {
    Settings s;
    auto str = [&](const char* sec, const char* name) {
        for (auto const& k : Schema())
            if (!strcmp(k.section, sec) && !strcmp(k.name, name)) return Get(v, k);
        return std::string();
    };
    auto num = [&](const char* sec, const char* name) { return atoi(str(sec, name).c_str()); };
    auto on = [&](const char* sec, const char* name) { return str(sec, name) == "on"; };
    auto surface = [&](const char* sec, Surface& o) {
        std::string e = str(sec, "effect");
        for (int i = 0; i < 6; i++) if (e == kEffects[i]) o.effect = static_cast<Effect>(i);
        o.speed = num(sec, "speed");
        o.motion = num(sec, "motion");
        o.opacity = num(sec, "opacity");
        o.blur = num(sec, "blur");
        std::string t = str(sec, "tint");
        o.hasTint = t.size() == 7;
        o.tint = o.hasTint ? Hex(t) : 0;
    };
    std::string th = str("general", "theme");
    s.theme = th == "dark" ? Theme::Dark : th == "light" ? Theme::Light : Theme::System;
    if (std::string a = str("general", "accent"); a.size() == 7) s.accent = Hex(a);
    s.reduceMotion = on("general", "reduce_motion");

    surface("taskbar", s.taskbar);
    s.taskbar.iconSize = num("taskbar", "icon_size");
    s.taskbar.magnification = num("taskbar", "magnification");
    s.taskbar.autohide = on("taskbar", "autohide");
    s.taskbar.hideDelay = num("taskbar", "hide_delay");
    s.taskbar.bounceOnLaunch = on("taskbar", "bounce_on_launch");
    s.taskbar.runningDots = on("taskbar", "running_dots");

    surface("controlcenter", s.cc);
    s.cc.hotCorner = on("controlcenter", "hot_corner");
    s.cc.cornerWidth = num("controlcenter", "corner_width");
    s.cc.cornerDelay = num("controlcenter", "corner_delay");
    s.cc.closeOnLeave = on("controlcenter", "close_on_leave");
    s.cc.showMedia = on("controlcenter", "show_media");
    s.cc.showSliders = on("controlcenter", "show_sliders");

    surface("start", s.start);
    s.start.recentApps = num("start", "recent_apps");
    s.start.recentFiles = num("start", "recent_files");
    s.start.dimDesktop = on("start", "dim_desktop");

    if (s.reduceMotion)
        for (Surface* o : {static_cast<Surface*>(&s.taskbar), static_cast<Surface*>(&s.cc), static_cast<Surface*>(&s.start)})
            if (o->effect != Effect::None) o->effect = Effect::Fade;
    return s;
}

bool Write(Values const& v) {
    std::string out = "; Halo settings. Edited by Halo Settings; see docs/SETTINGS.md for every key.\n";
    std::string section;
    for (auto const& k : Schema()) {
        if (section != k.section) {
            section = k.section;
            out += "\n[" + section + "]\n";
        }
        std::string sk = section + "." + k.name;
        auto it = v.find(sk);
        std::string val = it != v.end() ? Normalize(sk, it->second) : std::string();
        if (val.empty() && !(k.type == Type::Color && !*k.def)) val = k.def;
        out += std::string(k.name) + " = " + val + "\n";
    }
    CreateDirectoryW(Dir().c_str(), nullptr);
    std::wstring path = Path(), tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f) return false;
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp.c_str());  // read-only target, full disk: do not leave the half-written copy behind
    return ok;
}

const Settings& Get() {
    if (!g_loaded) Reload();
    return g_current;
}

bool Reload() {
    Values v = Read();
    bool changed = !g_loaded || v != g_values;
    g_values = std::move(v);
    g_current = Parse(g_values);
    g_loaded = true;
    return changed;
}

void Watch(HWND hwnd, UINT msg) {
    std::wstring dir = Path();  // the directory of the file actually read (HALO_SETTINGS_FILE moves it in tests)
    size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? Dir() : dir.substr(0, slash);
    CreateDirectoryW(dir.c_str(), nullptr);
    std::thread([hwnd, msg, dir] {
        HANDLE h = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
        if (h == INVALID_HANDLE_VALUE) return;
        while (IsWindow(hwnd) && WaitForSingleObject(h, INFINITE) == WAIT_OBJECT_0) {
            Sleep(150);  // debounce: a save is a write and a rename
            PostMessageW(hwnd, msg, 0, 0);
            if (!FindNextChangeNotification(h)) break;
        }
        FindCloseChangeNotification(h);
    }).detach();
}

}  // namespace halo::settings
