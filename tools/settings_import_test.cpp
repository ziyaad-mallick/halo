// Headless test of settings import (halo::settings::MergeImport): untrusted text in, schema-clean merged ini out.
// Pure string work, no files. Build with `tools/build.sh settings_import_test`; prints PASSED or FAILED.
#include <cstdio>
#include <string>

#include "../src/settings.h"

namespace st = halo::settings;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); g_fail++; } } while (0)

int wmain() {
    st::Values base = st::Defaults();
    base["taskbar.icon_size"] = "60";
    base["general.accent"] = "#112233";
    const std::string cur = st::Serialize(base);

    // Valid keys are applied (in their own section, any case/spacing/bool spelling); the rest is untouched.
    auto r = st::MergeImport(cur, "[General]\nTheme = DARK\nreduce_motion = yes\n[taskbar]\nspeed=150\naccent = #ABCDEF\n");
    st::Values v = st::ParseIni(r.text);
    CHECK(r.ok && r.applied == 3);
    CHECK(r.dropped == 1);  // accent is a general key, not a taskbar one
    CHECK(v["general.theme"] == "dark" && v["general.reduce_motion"] == "on" && v["taskbar.speed"] == "150");
    CHECK(v["general.accent"] == "#112233");  // not mentioned under [general]: kept
    CHECK(v["taskbar.icon_size"] == "60");    // not mentioned at all: kept

    // Unknown sections and keys are dropped, comments and blank lines are not counted.
    r = st::MergeImport(cur, "; a theme\n\n[evil]\nx = 1\n[general]\nnot_a_key = on\ntheme = light\ngarbage line\n");
    v = st::ParseIni(r.text);
    CHECK(r.ok && r.applied == 1 && r.dropped == 3);
    CHECK(v["general.theme"] == "light");
    CHECK(r.text.find("evil") == std::string::npos && r.text.find("not_a_key") == std::string::npos && r.text.find("garbage") == std::string::npos);

    // Out-of-range ints are clamped; non-numbers are dropped.
    auto const& schema = st::Schema();
    int lo = 0, hi = 0;
    for (auto const& k : schema) if (!std::string(k.section).compare("general") && !std::string(k.name).compare("contrast")) { lo = k.lo; hi = k.hi; }
    r = st::MergeImport(cur, "[general]\ncontrast = 999\nglass = -5\n[taskbar]\nspeed = fast\n");
    v = st::ParseIni(r.text);
    CHECK(r.applied == 2 && r.dropped == 1);
    CHECK(v["general.contrast"] == std::to_string(hi) && hi > lo);
    CHECK(v["general.glass"] == "0");
    CHECK(v["taskbar.speed"] == "100");  // dropped: stays at the current (default) value

    // Bad colours are dropped (the current colour stays); good ones are normalised to #RRGGBB upper-case.
    r = st::MergeImport(cur, "[general]\naccent = red\n");
    CHECK(r.applied == 0 && r.dropped == 1 && st::ParseIni(r.text)["general.accent"] == "#112233");
    for (const char* bad : {"#12345", "#1234567", "#GGGGGG", "112233", "#12 456", "rgb(1,2,3)"}) {
        r = st::MergeImport(cur, std::string("[general]\naccent = ") + bad + "\n");
        CHECK(r.applied == 0 && r.dropped == 1);
    }
    r = st::MergeImport(cur, "[general]\naccent = #aabbcc\n");
    CHECK(r.applied == 1 && st::ParseIni(r.text)["general.accent"] == "#AABBCC");

    // Markup and quotes never reach the file: such values are dropped, and a trailing comment is stripped.
    r = st::MergeImport(cur, "[general]\ntheme = <script>alert(1)</script>\naccent = \"#FF0000\"\nglass = 1\"><img src=x>\n[taskbar]\neffect = fade ;<script>x</script>\n");
    v = st::ParseIni(r.text);
    CHECK(r.applied == 1 && r.dropped == 3);
    CHECK(v["taskbar.effect"] == "fade");
    CHECK(r.text.find('<') == std::string::npos && r.text.find('"') == std::string::npos && r.text.find("script") == std::string::npos);

    // Oversized lines are dropped; the rest of the file still applies.
    r = st::MergeImport(cur, "[general]\ntheme = dark" + std::string(1000, ' ') + "; pad\nglass = 2\n");
    CHECK(r.ok && r.applied == 1 && r.dropped == 1 && st::ParseIni(r.text)["general.glass"] == "2");
    r = st::MergeImport(cur, "[general]\n" + std::string(5000, 'a') + " = on\n");
    CHECK(r.ok && r.applied == 0 && r.dropped == 1);

    // 64 KB is the cap: a 1 MB input is rejected outright, exactly 64 KB of valid-looking lines is not.
    r = st::MergeImport(cur, std::string(1 << 20, 'x'));
    CHECK(!r.ok && !r.error.empty() && r.text.empty() && r.applied == 0);
    std::string big = "[general]\n";
    while (big.size() < st::kMaxImport) big += "; padding padding padding padding padding\n";
    big.resize(st::kMaxImport);
    CHECK(st::MergeImport(cur, big).ok);
    CHECK(!st::MergeImport(cur, big + "x").ok);

    // Nothing mentioned (empty, binary junk, BOM-only): every current value survives.
    for (std::string in : {std::string(), std::string("\xEF\xBB\xBF"), std::string("\x00\x01\x02\xFF[", 5), std::string("[general]\n[taskbar]\n")}) {
        r = st::MergeImport(cur, in);
        CHECK(r.ok && r.applied == 0 && r.text == cur);
    }
    // Merging a file exported by Serialize() is idempotent and counts every key.
    r = st::MergeImport(cur, cur);
    CHECK(r.ok && r.applied == (int)schema.size() && r.dropped == 0 && r.text == cur);
    // A BOM and CRLF (a file saved by Notepad) are fine.
    r = st::MergeImport(cur, "\xEF\xBB\xBF[general]\r\ntheme = dark\r\n");
    CHECK(r.applied == 1 && st::ParseIni(r.text)["general.theme"] == "dark");

    // A header with a comment is still that header; a broken or over-long one switches sections off until the next good
    // one, so its keys never land in the section above it.
    r = st::MergeImport(cur, "[taskbar]\nspeed = 50\n[start] ; my start menu\nspeed = 200\n");
    v = st::ParseIni(r.text);
    CHECK(v["taskbar.speed"] == "50" && v["start.speed"] == "200");
    r = st::MergeImport(cur, "[taskbar]\nspeed = 50\n[start\nspeed = 70\n");
    v = st::ParseIni(r.text);
    CHECK(v["taskbar.speed"] == "50" && r.applied == 1 && r.dropped == 2);
    r = st::MergeImport(cur, "[taskbar]\nspeed = 50\n[" + std::string(300, 'x') + "]\nspeed = 60\n");
    v = st::ParseIni(r.text);
    CHECK(v["taskbar.speed"] == "50" && r.applied == 1);
    r = st::MergeImport(cur, "[taskbar]\nspeed = 50\n[]\nspeed = 80\n");
    CHECK(st::ParseIni(r.text)["taskbar.speed"] == "50");

    puts(g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
