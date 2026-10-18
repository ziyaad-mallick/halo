// Halo's design tokens: every size, radius and duration a panel uses comes from here; colours come from
// theme.h's palette. If a layout needs a value that is not on these scales, the layout is wrong.
//
//   Spacing  4 px base: 4 8 12 16 24.
//   Type     4 sizes on a ~1.25 ratio (12 caption, 14 body/title, 18 heading, 28 display), 2 weights.
//   Radius   2: small (controls, rows, chips) and large (tiles); a panel is large + its inset, so
//            corners stay concentric. Round controls are circles.
//   Motion   fast (state feedback) and slow (entrances); entering decelerates, leaving accelerates.
#pragma once

namespace halo::ui {

// Spacing.
constexpr float kS1 = 4, kS2 = 8, kS3 = 12, kS4 = 16, kS6 = 24;

// Type (logical px) and weights (DWRITE_FONT_WEIGHT values).
constexpr float kCaption = 12, kBody = 14, kHeading = 18, kDisplay = 28;
constexpr int kRegular = 400, kSemibold = 600;
constexpr wchar_t kFontText[] = L"Segoe UI Variable Text", kFontDisplay[] = L"Segoe UI Variable Display",
                  kFontIcons[] = L"Segoe Fluent Icons";
constexpr float kIcon = 16, kIconSmall = 14;  // glyph sizes

// Radius.
constexpr float kRadiusSmall = 12, kRadiusLarge = 20;
constexpr float kPanelInset = kS4, kPanelRadius = kRadiusLarge + kPanelInset;

// Control sizes (multiples of 4).
constexpr float kRound = 36;      // a round toggle inside a pill or tile
constexpr float kRoundAction = 44;  // the free-standing action buttons
constexpr float kPillH = 64;      // a connectivity pill (round toggle + two lines)
constexpr float kSliderH = 28;    // the thick slider track

// Motion.
constexpr int kFastMs = 120, kSlowMs = 280;
constexpr float kEnterTravel = 24;  // logical px a panel rises/sinks while it fades in/out

}  // namespace halo::ui
