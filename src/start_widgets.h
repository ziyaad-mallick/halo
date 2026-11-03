// Start 2.0 (settings.halo2): the pure parts. Quote of the day, the month grid, the greeting's first name,
// and the layout plan that makes the wider two-column panel fit the screen. No windows, no drawing: the
// launcher draws from these numbers and tools/start_test.cpp checks them headless.
//
// All sizes are logical px (the launcher multiplies by the monitor scale), on the 4 px spacing scale
// (src/tokens.h) except where a row has to match an existing 1.x row.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cwctype>
#include <string>

#include "tokens.h"

namespace halo::start {

// ---- Quote of the day ---------------------------------------------------------------------------------
// ~60 short quotes from authors long in the public domain, no network. The pick is a pure function of the date.
// Self-reliance additions: Emerson, Essays, First Series, "Self-Reliance":
// https://www.gutenberg.org/files/2944/2944-h/2944-h.htm
struct Quote {
    const wchar_t* text;
    const wchar_t* author;
};
inline constexpr Quote kQuotes[] = {
    {L"A journey of a thousand miles begins with a single step.", L"Lao Tzu"},
    {L"Trust thyself: every heart vibrates to that iron string.", L"Ralph Waldo Emerson"},
    {L"It does not matter how slowly you go as long as you do not stop.", L"Confucius"},
    {L"Learning without thought is labour lost.", L"Confucius"},
    {L"Know thyself.", L"Delphic maxim"},
    {L"Insist on yourself; never imitate.", L"Ralph Waldo Emerson"},
    {L"The beginning is the most important part of the work.", L"Plato"},
    {L"Well begun is half done.", L"Aristotle"},
    {L"No man ever steps in the same river twice.", L"Heraclitus"},
    {L"Give me a place to stand, and I will move the Earth.", L"Archimedes"},
    {L"Fortune favours the brave.", L"Virgil"},
    {L"While there is life, there is hope.", L"Cicero"},
    {L"Dripping water hollows out stone, not through force but through persistence.", L"Ovid"},
    {L"Seize the day.", L"Horace"},
    {L"Dare to be wise.", L"Horace"},
    {L"I am human; nothing human is alien to me.", L"Terence"},
    {L"A rolling stone gathers no moss.", L"Publilius Syrus"},
    {L"Do your work, and you shall reinforce yourself.", L"Ralph Waldo Emerson"},
    {L"While we are postponing, life speeds by.", L"Seneca"},
    {L"First say to yourself what you would be; then do what you have to do.", L"Epictetus"},
    {L"The soul becomes dyed with the colour of its thoughts.", L"Marcus Aurelius"},
    {L"If it is not right, do not do it; if it is not true, do not say it.", L"Marcus Aurelius"},
    {L"Nothing can bring you peace but yourself.", L"Ralph Waldo Emerson"},
    {L"Slow and steady wins the race.", L"Aesop"},
    {L"United we stand, divided we fall.", L"Aesop"},
    {L"Look before you leap.", L"Aesop"},
    {L"It is easy to be brave from a safe distance.", L"Aesop"},
    {L"If I have seen further, it is by standing on the shoulders of giants.", L"Isaac Newton"},
    {L"All truths are easy to understand once they are discovered; the point is to discover them.", L"Galileo"},
    {L"A man who dares to waste one hour of time has not discovered the value of life.", L"Charles Darwin"},
    {L"Nothing in life is to be feared, it is only to be understood.", L"Marie Curie"},
    {L"Genius is one percent inspiration and ninety-nine percent perspiration.", L"Thomas Edison"},
    {L"To be great is to be misunderstood.", L"Ralph Waldo Emerson"},
    {L"To thine own self be true.", L"Shakespeare"},
    {L"We know what we are, but know not what we may be.", L"Shakespeare"},
    {L"Our doubts are traitors.", L"Shakespeare"},
    {L"To strive, to seek, to find, and not to yield.", L"Tennyson"},
    {L"Do that which is assigned you, and you cannot hope too much or dare too much.", L"Ralph Waldo Emerson"},
    {L"Nothing will ever be attempted if all possible objections must first be overcome.", L"Samuel Johnson"},
    {L"The perfect is the enemy of the good.", L"Voltaire"},
    {L"Nothing can bring you peace but the triumph of principles.", L"Ralph Waldo Emerson"},
    {L"Well done is better than well said.", L"Benjamin Franklin"},
    {L"Lost time is never found again.", L"Benjamin Franklin"},
    {L"An investment in knowledge pays the best interest.", L"Benjamin Franklin"},
    {L"Either write something worth reading or do something worth writing.", L"Benjamin Franklin"},
    {L"I am a slow walker, but I never walk back.", L"Abraham Lincoln"},
    {L"Nothing great was ever achieved without enthusiasm.", L"Ralph Waldo Emerson"},
    {L"Do the thing and you shall have the power.", L"Ralph Waldo Emerson"},
    {L"Make the most of yourself, for that is all there is of you.", L"Ralph Waldo Emerson"},
    {L"Simplify, simplify.", L"Henry David Thoreau"},
    {L"The price of anything is the amount of life you exchange for it.", L"Henry David Thoreau"},
    {L"It's not what you look at that matters, it's what you see.", L"Henry David Thoreau"},
    {L"Great is the art of beginning, but greater the art of ending.", L"Longfellow"},
    {L"Keep your face always toward the sunshine, and shadows will fall behind you.", L"Walt Whitman"},
    {L"Courage is resistance to fear, mastery of fear, not absence of fear.", L"Mark Twain"},
    {L"Knowing is not enough; we must apply.", L"Goethe"},
    {L"Everyone thinks of changing the world, but no one thinks of changing himself.", L"Leo Tolstoy"},
    {L"He who has a why to live can bear almost any how.", L"Friedrich Nietzsche"},
    {L"You cannot cross the sea merely by standing and staring at the water.", L"Rabindranath Tagore"},
    {L"Whether you think you can, or you think you can't, you're right.", L"Henry Ford"},
    {L"The only thing we have to fear is fear itself.", L"Franklin D. Roosevelt"},
};
inline constexpr int kQuoteCount = static_cast<int>(sizeof(kQuotes) / sizeof(kQuotes[0]));

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's algorithm).
inline long DaysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = static_cast<unsigned>((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<long>(doe) - 719468;
}

// 0 = Sunday .. 6 = Saturday.
inline int Weekday(int y, int m, int d) {
    const long days = DaysFromCivil(y, m, d);
    return static_cast<int>(((days + 4) % 7 + 7) % 7);  // 1970-01-01 was a Thursday
}

inline int DaysInMonth(int y, int m) {
    static constexpr int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : dim[(m - 1) % 12];
}

// The quote for a date: the same all day, the next one tomorrow.
inline int QuoteIndex(int y, int m, int d) {
    const long n = DaysFromCivil(y, m, d) % kQuoteCount;
    return static_cast<int>(n < 0 ? n + kQuoteCount : n);
}
inline Quote QuoteForDate(int y, int m, int d) { return kQuotes[QuoteIndex(y, m, d)]; }

// ---- Greeting -----------------------------------------------------------------------------------------
// "Hello, <first name>": the first word of the account's display name, else of the user name; a name that
// is all lower case ("testi") gets its first letter raised.
inline std::wstring FirstName(std::wstring const& display, std::wstring const& userName) {
    auto first = [](std::wstring s) {
        const auto at = s.find(L'@');  // a Microsoft account named by its address
        if (at != std::wstring::npos) s.resize(at);
        const auto b = s.find_first_not_of(L" \t");
        if (b == std::wstring::npos) return std::wstring();
        const auto e = s.find_first_of(L" \t", b);
        return s.substr(b, e == std::wstring::npos ? std::wstring::npos : e - b);
    };
    std::wstring n = first(display);
    if (n.empty()) n = first(userName);
    if (n.empty()) return L"there";
    if (std::none_of(n.begin(), n.end(), [](wchar_t c) { return std::iswupper(c) != 0; })) n[0] = static_cast<wchar_t>(std::towupper(n[0]));
    return n;
}

// ---- Month grid ---------------------------------------------------------------------------------------
struct MonthGrid {
    int rows = 0;                   // 4 to 6
    std::array<int, 42> day{};      // row-major, 7 columns; 0 = blank
    int todayCell = -1;             // index into `day`, -1 when `today` is not in the month
};
// `firstDow`: the column-0 weekday, 0 = Sunday (Monday-first locales pass 1).
inline MonthGrid MakeMonthGrid(int y, int m, int today, int firstDow) {
    MonthGrid g;
    const int lead = (Weekday(y, m, 1) - firstDow + 7) % 7, dim = DaysInMonth(y, m);
    g.rows = (lead + dim + 6) / 7;
    for (int d = 1; d <= dim; d++) g.day[lead + d - 1] = d;
    if (today >= 1 && today <= dim) g.todayCell = lead + today - 1;
    return g;
}

// ---- Tokens -------------------------------------------------------------------------------------------
// Everything Start 2.0 draws comes from these, which come from tokens.h. Type: 4 sizes, 2 weights. Spacing: 8 px inside
// a card, 16 between items of a group, 24 between groups. Radii: panel, then one for cards and tiles, one for rows
// inside them; a pill, chip, badge or slider is fully round. Elevation: the panel and the Power menu cast a
// shadow, nothing flat does. Motion: fast (feedback) and slow (entrances), nothing overshoots.
namespace tk {
constexpr float kHero = ui::kDisplay, kTitle = ui::kHeading, kText = ui::kBody, kCap = ui::kCaption;
constexpr float kInCard = ui::kS2, kPad = ui::kS3, kInGroup = ui::kS4, kGroup = ui::kS6, kMargin = ui::kS6;
constexpr float kRCard = ui::kRadiusLarge, kRRow = ui::kRadiusSmall, kRPanel = ui::kPanelRadius;
constexpr float kTile = 56, kBadge = 32, kTrack = 28, kCardH = 88, kAppCellH = 92, kAppIcon = 48, kFileRowH = 34, kFileIcon = 20,
                kFileRows = 5, kSeg = 24, kMedia = 72, kMediaArt = 48, kMediaBtn = 32, kUsageRow = 22, kChevBtn = 28;
constexpr float kMenuShadowBlur = 24, kMenuShadowDy = 8;  // the one raised surface besides the panel: the Power menu
constexpr int kFastMs = ui::kFastMs, kSlowMs = ui::kSlowMs;
}  // namespace tk

// ---- Layout -------------------------------------------------------------------------------------------
inline constexpr float kScreenMargin = 24, kPanelTop = 8;
inline constexpr float kLeftMax = 664, kLeftMin = 480, kRightW = 330, kRightMinW = 290;
inline constexpr float kSearchTop = 252, kSearchH = 44, kGap = 26, kLabelH = 16;
inline constexpr float kAppRowH = tk::kAppCellH, kFileCellH = tk::kFileRowH, kBottomH = 56;
inline constexpr float kGreetQuoteH = 84, kGreetH = 36;
inline constexpr float kGreetingClockW = 96;
inline constexpr float kWidgetsH = tk::kCardH;
inline constexpr float kRightPad = 14, kRightMinH = 336;

struct Widths {
    float left, right, total;
};
// Left and right column widths for a monitor `monitorW` logical px wide. Shrinks the left column to kLeftMin first,
// then the right to kRightMinW; on a screen narrower than that the panel is simply as wide as the minimum.
inline Widths PlanWidths(float monitorW) {
    const float avail = monitorW - 2 * kScreenMargin;
    float right = kRightW, left = std::min(kLeftMax, avail - right);
    if (left < kLeftMin) {
        left = kLeftMin;
        right = std::max(kRightMinW, avail - left);
    }
    return {left, right, left + right};
}

// The tallest the panel may be: from the monitor's top to the panel's bottom edge (the anchor), less the room
// above for the spring and a margin.
inline float AvailHeight(float monitorTopPx, float anchorBottomPx, float scale) {
    return (anchorBottomPx - monitorTopPx) / scale - kPanelTop - 8;
}

// Greeting, two cards, search, two rows of apps and six rows of files (two columns); recent content scrolls when needed.
struct LeftPlan {
    int level = 0;
    bool quote = true, files = true, weekOnly = true;
    float greetY = 32, greetH = kGreetQuoteH, widgetsY = 140, widgetsH = kWidgetsH;
    float searchY = kSearchTop;
    float appsLabelY = 0, appsTop = 0, filesLabelY = 0, filesTop = 0;
    float dividerY = 0, height = 0, contentBottom = 0;
};
inline LeftPlan PlanLeft(float avail, bool wantApps, bool wantFiles) {
    LeftPlan p;
    p.files = wantFiles;
    if (avail < 400) {  // keep the search/results reachable on short, high-DPI displays
        p.greetH -= tk::kGroup;
        p.widgetsY -= tk::kGroup;
        p.searchY -= tk::kGroup;
    }
    float y = p.searchY + kSearchH + kGap;
    p.appsLabelY = y;
    p.appsTop = y + kLabelH + tk::kInCard;
    if (wantApps) y = p.appsTop + 2 * kAppRowH + kGap;
    p.filesLabelY = y;
    p.filesTop = y + kLabelH + tk::kInCard;
    if (wantFiles) y = p.filesTop + tk::kFileRows * tk::kFileRowH + tk::kInGroup;
    p.contentBottom = y;
    p.height = std::min(avail, y + 4 + kBottomH);
    p.dividerY = p.height - kBottomH - 4;
    return p;
}

// ---- Three columns: notifications + calendar | search + apps | control centre ------------------------------------
// The panel is `notif + mid + right` wide. Narrow screens shrink the control centre first, then the notification
// column, never the middle (search and apps are what Start is for). On a screen too small for all three the
// notification column is dropped (notif = 0).
inline constexpr float kNotifW = 256, kNotifMinW = 230, kMidW = 440, kMidMinW = 380, kPanelGap = 12;  // 1050 wide in all: the notification pane stays, the middle and control panes are fat
struct Widths3 {
    float notif, mid, right, total;
};
inline Widths3 PlanWidths3(float monitorW) {
    const float avail = monitorW - 2 * kScreenMargin - 2 * kPanelGap;  // the card widths, without the gaps between them
    float notif = kNotifW, right = kRightW, mid = std::min(kMidW, avail - notif - right);
    if (mid < kMidMinW) {
        mid = kMidMinW;
        right = std::max(kRightMinW, avail - notif - mid);
        if (notif + mid + right > avail) notif = std::max(kNotifMinW, avail - mid - right);
        if (notif + mid + right > avail) notif = 0;  // a screen this small keeps Start and the control centre, and drops the column
    }
    return {notif, mid, right, notif + mid + right};
}

// The middle column's plan, top to bottom: the greeting and a quote, a slim strip (battery and power mode), search,
// two rows of apps, files. Same fields as the two-column plan, so the Launcher keeps one shape of plan.
inline constexpr float kHeroTop = 24;
inline LeftPlan PlanCenter(float avail, bool wantApps, bool wantFiles) {
    LeftPlan p;
    p.files = wantFiles;
    p.greetY = kHeroTop;
    p.greetH = kGreetQuoteH;
    if (avail < 560) {  // short, high-DPI screens: a tighter hero (the quote drops to a compact line)
        p.greetY = 20;
        p.greetH = kGreetQuoteH - tk::kGroup + 8;
    }
    p.widgetsY = p.greetY + p.greetH;  // no strip any more (the battery bar is in the control pane)
    p.widgetsH = 0;
    p.searchY = p.greetY + p.greetH + 14;
    float y = p.searchY + kSearchH + kGap;
    p.appsLabelY = y;
    p.appsTop = y + kLabelH + tk::kInCard;
    if (wantApps) y = p.appsTop + 2 * kAppRowH + kGap;  // 5 x 2 apps
    p.filesLabelY = y;
    p.filesTop = y + kLabelH + tk::kInCard;
    if (wantFiles) y = p.filesTop + tk::kFileRows * tk::kFileRowH + tk::kInGroup;
    p.contentBottom = y;
    p.height = std::min(avail, y + 4 + kBottomH);
    p.dividerY = p.height - kBottomH - 4;
    return p;
}

// The right column (a compact control centre) for a panel `height` tall. Everything is a y from the top of the panel.
// Four rows of tiles (Wi-Fi/Bluetooth, Airplane/Hotspot, Do not disturb/Night light, Dark mode/Microphone), the
// Display and Sound sliders, Now Playing, the usage grid. Spare height widens the gaps between groups a little.
struct RightPlan {
    bool roomy = true;
    int rows = 4;
    float rowY[6] = {24, 88, 152, 216, 280, 344}, rowH = tk::kTile, rowGap = tk::kInCard;
    float tilesY = 24, tilesH = tk::kTile;  // the first row
    float brightY = 0, volY = 0, sliderH = tk::kTrack;
    float mediaY = 0, mediaH = tk::kMedia;
    float battY = 0, battH = 0;  // the compact plan's battery bar: battery and power mode, between the tiles and the sliders
    float usageY = 0, usageH = kLabelH + 6 + 2 * tk::kUsageRow;
    float gap = tk::kGroup, bottom = 0;
};
// `rows` tile rows (the 2.0 panel has 6: Wi-Fi/Bluetooth, then ten quick tiles in pairs); `compact` is the small 2.0 panel's sizes.
inline RightPlan PlanRight(float height, int rows = 4, bool compact = false) {
    RightPlan r;
    r.rows = rows;
    float top = 24;
    if (compact) {
        r.rowH = 48; r.rowGap = 8; r.sliderH = 38; r.mediaH = 60; r.gap = 14; top = 18; r.battH = 44;
        for (int i = 0; i < rows; i++) r.rowY[i] = top + i * (r.rowH + r.rowGap);
        r.tilesY = top; r.tilesH = r.rowH;
        const float rowsEnd = r.rowY[rows - 1] + r.rowH;
        r.battY = rowsEnd + r.gap;
        r.brightY = r.battY + r.battH + r.gap;
        r.volY = r.brightY + r.sliderH + r.rowGap;
        r.mediaY = r.volY + r.sliderH + r.gap;
        r.usageY = r.mediaY + r.mediaH + r.gap;
        r.bottom = r.usageY + r.usageH + kRightPad;
        return r;
    }
    auto total = [&](float rowH, float gap) {
        return 24 + 4 * rowH + 3 * r.rowGap + 3 * gap + 2 * r.sliderH + r.rowGap + r.mediaH + r.usageH + kRightPad;
    };
    float extra = std::max(0.0f, height - kBottomH - 1 - total(r.rowH, tk::kGroup));
    if (extra >= 32) {  // room to spare: taller tiles first, then wider gaps between groups
        r.rowH = 64;
        extra -= 32;
    }
    r.gap = tk::kGroup + std::min(extra / 3, 12.0f);
    for (int i = 0; i < 4; i++) r.rowY[i] = 24 + i * (r.rowH + r.rowGap);
    r.tilesH = r.rowH;
    const float rowsEnd = r.rowY[3] + r.rowH;
    r.brightY = rowsEnd + r.gap;
    r.volY = r.brightY + r.sliderH + r.rowGap;
    r.mediaY = r.volY + r.sliderH + r.gap;
    r.usageY = r.mediaY + r.mediaH + r.gap;
    r.bottom = r.usageY + r.usageH + kRightPad;
    return r;
}

}  // namespace halo::start
