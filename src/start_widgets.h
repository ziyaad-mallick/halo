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

namespace halo::start {

// ---- Quote of the day ---------------------------------------------------------------------------------
// ~60 short quotes from authors long in the public domain, no network. The pick is a pure function of the date.
struct Quote {
    const wchar_t* text;
    const wchar_t* author;
};
inline constexpr Quote kQuotes[] = {
    {L"A journey of a thousand miles begins with a single step.", L"Lao Tzu"},
    {L"Nature does not hurry, yet everything is accomplished.", L"Lao Tzu"},
    {L"It does not matter how slowly you go as long as you do not stop.", L"Confucius"},
    {L"Learning without thought is labour lost.", L"Confucius"},
    {L"Know thyself.", L"Delphic maxim"},
    {L"The unexamined life is not worth living.", L"Socrates"},
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
    {L"It is a bad plan that admits of no modification.", L"Publilius Syrus"},
    {L"While we are postponing, life speeds by.", L"Seneca"},
    {L"First say to yourself what you would be; then do what you have to do.", L"Epictetus"},
    {L"The soul becomes dyed with the colour of its thoughts.", L"Marcus Aurelius"},
    {L"If it is not right, do not do it; if it is not true, do not say it.", L"Marcus Aurelius"},
    {L"Let your plans be dark and impenetrable as night, and when you move, fall like a thunderbolt.", L"Sun Tzu"},
    {L"Slow and steady wins the race.", L"Aesop"},
    {L"United we stand, divided we fall.", L"Aesop"},
    {L"Look before you leap.", L"Aesop"},
    {L"It is easy to be brave from a safe distance.", L"Aesop"},
    {L"If I have seen further, it is by standing on the shoulders of giants.", L"Isaac Newton"},
    {L"All truths are easy to understand once they are discovered; the point is to discover them.", L"Galileo"},
    {L"A man who dares to waste one hour of time has not discovered the value of life.", L"Charles Darwin"},
    {L"Nothing in life is to be feared, it is only to be understood.", L"Marie Curie"},
    {L"Genius is one percent inspiration and ninety-nine percent perspiration.", L"Thomas Edison"},
    {L"Brevity is the soul of wit.", L"Shakespeare"},
    {L"To thine own self be true.", L"Shakespeare"},
    {L"We know what we are, but know not what we may be.", L"Shakespeare"},
    {L"Our doubts are traitors.", L"Shakespeare"},
    {L"To strive, to seek, to find, and not to yield.", L"Tennyson"},
    {L"No man is an island.", L"John Donne"},
    {L"Nothing will ever be attempted if all possible objections must first be overcome.", L"Samuel Johnson"},
    {L"The perfect is the enemy of the good.", L"Voltaire"},
    {L"Doubt is not a pleasant condition, but certainty is absurd.", L"Voltaire"},
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

// ---- Layout -------------------------------------------------------------------------------------------
inline constexpr float kScreenMargin = 24, kPanelTop = 8;  // as launcher.cpp: margin to the screen edge, room above for the spring
inline constexpr float kLeftMax = 540, kLeftMin = 480, kRightW = 312, kRightMinW = 264;
inline constexpr float kSearchTop = 16, kSearchH = 40, kGap = 10, kLabelH = 16;
inline constexpr float kAppRowH = 64, kFileCellH = 70, kBottomH = 56;
inline constexpr float kGreetQuoteH = 40, kGreetH = 24;     // "Hello, Name" + quote line / the greeting alone
inline constexpr float kWidgetsH = 120, kWidgetsCompactH = 64;
inline constexpr float kRightPad = 16, kRightMinH = 336;

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

// The left column, top to bottom, as y's from the top of the panel. Levels, from roomiest to tightest:
//   0 greeting + quote, widgets (clock, month, battery, power mode), recent apps, recent files
//   1 the same without recent files
//   2 the greeting without the quote
//   3 the month shrinks to this week
// The first level whose height fits `avail` wins; level 3 is used if even that does not.
struct LeftPlan {
    int level = 0;
    bool quote = true, files = true, weekOnly = false;
    float greetY = 0, greetH = 0, widgetsY = 0, widgetsH = 0;
    float appsLabelY = 0, appsTop = 0, filesLabelY = 0, filesTop = 0;
    float dividerY = 0, height = 0;
};
inline LeftPlan PlanLeft(float avail, bool wantApps, bool wantFiles) {
    LeftPlan p;
    for (int level = 0; level <= 3; level++) {
        p = LeftPlan{};
        p.level = level;
        p.quote = level <= 1;
        p.files = wantFiles && level == 0;
        p.weekOnly = level == 3;
        float y = kSearchTop + kSearchH + kGap;
        p.greetY = y;
        p.greetH = p.quote ? kGreetQuoteH : kGreetH;
        y += p.greetH + kGap;
        p.widgetsY = y;
        p.widgetsH = p.weekOnly ? kWidgetsCompactH : kWidgetsH;
        y += p.widgetsH + kGap;
        p.appsLabelY = y;  // also where the "ALL APPS" header of the list goes when the section itself is off
        p.appsTop = y + kLabelH + 6;
        if (wantApps) y = p.appsTop + kAppRowH + kGap;
        if (p.files) {
            p.filesLabelY = y;
            p.filesTop = y + kLabelH + 6;
            y = p.filesTop + kFileCellH + kGap;
        }
        p.dividerY = y;
        p.height = y + 1 + kBottomH;
        if (p.height <= avail) break;
    }
    return p;
}

// The right column (a compact control centre) for a panel `height` tall. Everything is a y from the top of the panel.
struct RightPlan {
    bool roomy = false;  // taller media card and the usage monitor as four rows instead of a 2 x 2 grid
    float tilesY = 0, tilesH = 56;
    float brightY = 0, volY = 0, sliderH = 24;
    float mediaY = 0, mediaH = 0;
    float usageY = 0, usageH = 0;
    float gap = 0, bottom = 0;  // bottom: the lowest y used
};
inline RightPlan PlanRight(float height) {
    RightPlan r;
    const float tiles = 56, slider = 24, sliderGap = 8;
    const float mediaC = 56, usageC = kLabelH + 6 + 2 * (14 + 4) + 10;  // 68: a label and a 2 x 2 grid
    const float mediaR = 88, usageR = kLabelH + 6 + 4 * 28;            // 134: a label and four rows
    const float fixedC = tiles + 2 * slider + sliderGap + mediaC + usageC;
    const float fixedR = tiles + 2 * slider + sliderGap + mediaR + usageR;
    r.roomy = height - 2 * kRightPad - fixedR >= 4 * 12;
    const float fixed = r.roomy ? fixedR : fixedC;
    r.mediaH = r.roomy ? mediaR : mediaC;
    r.usageH = r.roomy ? usageR : usageC;
    r.tilesH = tiles;
    r.sliderH = slider;
    r.gap = std::clamp((height - 2 * kRightPad - fixed) / 4, 4.0f, r.roomy ? 32.0f : 16.0f);
    float y = kRightPad;
    r.tilesY = y;
    y += tiles + r.gap;
    r.brightY = y;
    r.volY = y + slider + sliderGap;
    y = r.volY + slider + r.gap;
    r.mediaY = y;
    y += r.mediaH + r.gap;
    r.usageY = y;
    r.bottom = y + r.usageH;
    return r;
}

}  // namespace halo::start
