// Headless test for Start 2.0: quote selection, month grid, first name, layout fit on the small laptop
// screens, and the usage sampler (pure parsing plus a few seconds of live PDH). Build: tools/build.sh start_test.
// Exits 0 when everything holds; prints what it measured.
#include "../src/start_widgets.h"
#include "../src/usage.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

using namespace halo;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) {                                                               \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                      \
            g_fail++;                                                             \
        }                                                                         \
    } while (0)

static void Quotes() {
    CHECK(start::kQuoteCount >= 55 && start::kQuoteCount <= 70);
    for (int i = 0; i < start::kQuoteCount; i++) {
        std::wstring t = start::kQuotes[i].text, a = start::kQuotes[i].author;
        CHECK(!t.empty() && !a.empty());
        CHECK(t.size() <= 100);  // one line in the strip
    }
    // Deterministic: the same date, the same quote; the next day, the next quote; a full cycle covers them all.
    auto a = start::QuoteForDate(2026, 10, 21), b = start::QuoteForDate(2026, 10, 21);
    CHECK(a.text == b.text);
    CHECK(start::QuoteIndex(2026, 10, 22) == (start::QuoteIndex(2026, 10, 21) + 1) % start::kQuoteCount);
    std::set<int> seen;
    for (int d = 1; d <= start::kQuoteCount; d++) seen.insert(start::QuoteIndex(2026, 1, d));
    CHECK(static_cast<int>(seen.size()) == start::kQuoteCount);
    for (int y : {1969, 1970, 2026, 2100})
        for (int m = 1; m <= 12; m++) {
            int i = start::QuoteIndex(y, m, 1);
            CHECK(i >= 0 && i < start::kQuoteCount);
        }
    std::printf("quotes: %d, 2026-10-21 -> #%d\n", start::kQuoteCount, start::QuoteIndex(2026, 10, 21));
}

static void Calendar() {
    CHECK(start::Weekday(2026, 10, 21) == 3);  // Wednesday
    CHECK(start::Weekday(1970, 1, 1) == 4);    // Thursday
    CHECK(start::DaysInMonth(2024, 2) == 29 && start::DaysInMonth(2100, 2) == 28 && start::DaysInMonth(2026, 10) == 31);
    auto g = start::MakeMonthGrid(2026, 10, 21, 0);  // Oct 1 is a Thursday: 4 blanks, Sunday first
    CHECK(g.rows == 5 && g.day[4] == 1 && g.day[0] == 0 && g.todayCell == 24 && g.day[24] == 21);
    auto m = start::MakeMonthGrid(2026, 10, 21, 1);  // Monday first: 3 blanks
    CHECK(m.rows == 5 && m.day[3] == 1 && m.todayCell == 23);
    CHECK(start::MakeMonthGrid(2026, 2, 0, 0).rows == 4);  // Feb 2026 starts on a Sunday and has 28 days
    CHECK(start::MakeMonthGrid(2026, 2, 0, 0).todayCell == -1);
    CHECK(start::MakeMonthGrid(2027, 5, 1, 0).rows == 6);  // May 2027: Saturday start, 31 days
}

static void Names() {
    CHECK(start::FirstName(L"Ziyaad Mallick", L"testi") == L"Ziyaad");
    CHECK(start::FirstName(L"", L"testi") == L"Testi");
    CHECK(start::FirstName(L"  ", L"") == L"there");
    CHECK(start::FirstName(L"ziya@example.com", L"x") == L"Ziya");
    CHECK(start::FirstName(L"Mary-Jane Q", L"") == L"Mary-Jane");
}

// A screen like the Lenovo's: 1366x768 at a scale, the dock's pill taking ~90 logical px under the panel.
static void Layout() {
    struct Case {
        float w, h, scale;
    } cases[] = {{1366, 768, 1.25f}, {1366, 768, 1.5f}, {1366, 768, 1.0f}, {1920, 1080, 1.0f}, {1920, 1080, 1.5f}, {1280, 720, 1.5f}};
    for (auto c : cases) {
        const float anchorBottom = c.h - 90 * c.scale;
        const float avail = start::AvailHeight(0, anchorBottom, c.scale);
        auto plan = start::PlanLeft(avail, true, true);
        const float height = std::max(plan.height, start::kRightMinH);
        auto widths = start::PlanWidths(c.w / c.scale);
        auto right = start::PlanRight(height);
        std::printf("%gx%g @%g%%: avail %.0f -> level %d, height %.0f, width %.0f+%.0f=%.0f of %.0f, right gap %.0f%s\n", c.w, c.h,
                    c.scale * 100, avail, plan.level, height, widths.left, widths.right, widths.total, c.w / c.scale, right.gap,
                    right.roomy ? " roomy" : "");
        CHECK(height <= avail);                                         // it fits vertically: shrunk, not clipped
        CHECK(widths.total + 2 * start::kScreenMargin <= c.w / c.scale + 0.01f);  // and horizontally
        CHECK(right.bottom <= height - start::kRightPad + 0.01f);       // the right column fits its own height
        CHECK(right.tilesY >= 0 && right.brightY > right.tilesY && right.volY > right.brightY && right.mediaY > right.volY &&
              right.usageY > right.mediaY);
        CHECK(plan.greetY < plan.widgetsY);
        CHECK(plan.widgetsY + plan.widgetsH <= plan.searchY);
        CHECK(plan.searchY + start::kSearchH < plan.appsLabelY);
        CHECK(plan.files && plan.quote && plan.weekOnly);
        CHECK(plan.filesTop >= plan.appsTop + 2 * start::kAppRowH);
        CHECK(plan.contentBottom >= plan.filesTop + 74 + start::kFileCellH);
        CHECK(plan.dividerY > plan.searchY + start::kSearchH + 34);
        // Every one of the 12 files is reachable by scrolling; never drop a section for height.
        const float maxScroll = std::max(0.0f, plan.contentBottom - plan.dividerY);
        CHECK(plan.filesTop + 74 + start::kFileCellH - maxScroll <= plan.dividerY);
    }
    for (float avail = 700; avail >= 336; avail -= 4) {
        auto p = start::PlanLeft(avail, true, true);
        CHECK(p.height <= avail && p.files && p.quote && p.weekOnly);
        CHECK(p.contentBottom >= p.filesTop + 74 + start::kFileCellH);
    }
    // The right column holds for every panel height the left can produce.
    for (float h = start::kRightMinH; h <= 560; h += 4) CHECK(start::PlanRight(h).bottom <= h - start::kRightPad + 0.01f);
}

static void UsageParsing() {
    using namespace usage;
    CHECK(ClampPct(-5) == 0 && ClampPct(250) == 100 && ClampPct(42.5) == 42.5f && ClampPct(std::nan("")) == 0);
    const std::wstring a = L"pid_1234_luid_0x00000000_0x0000F1B2_phys_0_eng_3_engtype_3D";
    CHECK(GpuAdapterOf(a) == L"luid_0x00000000_0x0000F1B2_phys_0");
    CHECK(GpuEngineOf(a) == L"eng_3_engtype_3D");
    CHECK(GpuAdapterOf(L"garbage").empty() && GpuEngineOf(L"garbage").empty());
    std::vector<std::pair<std::wstring, double>> items = {
        {L"pid_1_luid_0x0_0xA_phys_0_eng_0_engtype_3D", 30},      {L"pid_2_luid_0x0_0xA_phys_0_eng_0_engtype_3D", 25},  // same engine: 55
        {L"pid_1_luid_0x0_0xA_phys_0_eng_1_engtype_Copy", 40},    {L"pid_3_luid_0x0_0xB_phys_0_eng_0_engtype_3D", 70},  // other adapter
        {L"junk", 99},
    };
    CHECK(GpuFigure(items) == 70);  // the busiest engine of the busiest adapter
    items[3].second = 5;
    CHECK(GpuFigure(items) == 55);  // two processes on one engine add up
    items.push_back({L"pid_9_luid_0x0_0xA_phys_0_eng_0_engtype_3D", 90});
    CHECK(GpuFigure(items) == 100);  // clamped
    CHECK(GpuFigure({}) == 0);
    float ram = usage::RamPercent();
    CHECK(ram > 0 && ram <= 100);
}

static void UsageLive() {
    HWND w = CreateWindowExW(0, L"STATIC", L"start_test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    CHECK(w != nullptr);
    constexpr UINT kMsg = WM_APP + 1;
    usage::Sampler s;
    CHECK(!s.Running());
    s.Start(w, kMsg);
    CHECK(s.Running());
    s.Start(w, kMsg);  // a second Start is a no-op
    int got = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (got < 3 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(8)) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 200, QS_ALLPOSTMESSAGE);
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
            if (m.message == kMsg) got++;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto u = s.Latest();
    std::printf("sampler: %d samples in %.1f s; cpu %.1f%% (%d) gpu %.1f%% (%d) disk %.1f%% (%d) ram %.1f%%\n", got, secs, u.cpu,
                u.hasCpu, u.gpu, u.hasGpu, u.disk, u.hasDisk, u.ram);
    CHECK(got >= 3);
    CHECK(secs >= 2.5 && secs < 6);  // ~1 Hz, not a busy loop and not stalled
    CHECK(u.hasCpu && u.cpu >= 0 && u.cpu <= 100);
    CHECK(u.hasDisk && u.disk >= 0 && u.disk <= 100);
    CHECK(u.gpu >= 0 && u.gpu <= 100);  // hasGpu may be false on a machine without the counter set
    CHECK(u.ram > 0 && u.ram <= 100);
    auto s0 = std::chrono::steady_clock::now();
    s.Stop();
    const double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - s0).count();
    std::printf("sampler stop: %.0f ms\n", stopMs);
    CHECK(!s.Running());
    CHECK(stopMs < 500);  // closing Start does not wait for the second to run out
    // Closed means closed: nothing arrives after Stop().
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {}
    Sleep(1300);
    int late = 0;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
        if (m.message == kMsg) late++;
    CHECK(late == 0);
    s.Start(w, kMsg);  // and it can be reopened
    CHECK(s.Running());
    s.Stop();
    DestroyWindow(w);
}

int wmain() {
    Quotes();
    Calendar();
    Names();
    Layout();
    UsageParsing();
    UsageLive();
    std::printf(g_fail ? "start_test: %d FAILED\n" : "start_test: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
