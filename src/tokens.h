// Halo's design tokens: every size, radius and duration a panel uses comes from here; colours come from
// theme.h's palette. If a layout needs a value that is not on these scales, the layout is wrong.
//
//   Spacing  4 px base: 4 8 12 16 24.
//   Type     4 sizes on a ~1.25 ratio (12 caption, 14 body/title, 18 heading, 28 display), 2 weights.
//   Radius   2: small (controls, rows, chips) and large (tiles); a panel is large + its inset, so
//            corners stay concentric. Round controls are circles.
//   Motion   fast (state feedback) and slow (entrances); entering decelerates, leaving accelerates. A press
//            answers at once (no animation is waited on), and a slider drag tracks the pointer 1:1.
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
constexpr float kSliderH = 22;    // the slider track as drawn (was 28: ~21 % shorter)
constexpr float kSliderHitH = kSliderH + 2 * kS1;  // its pointer target: 4 px taller on each side, so it is still easy to grab
constexpr float kMediaArt = 48;   // now-playing artwork
constexpr float kMediaH = kMediaArt + 2 * kS3;  // the now-playing module

// Notification Center: compact floating cards, a quiet heading, then a calendar widget.
constexpr float kNotificationWidth = 360, kNotificationEdge = kS4;
constexpr float kNotificationPad = kS4, kNotificationGap = kS3;
constexpr float kNotificationRadius = kRadiusLarge;
constexpr float kNotificationCalendarHead = kS4, kNotificationCalendarRow = 28;

// Motion.
constexpr int kFastMs = 90, kSlowMs = 280;  // fast was 120
constexpr int kHoverMs = 80;                // a highlight gliding between controls, and fading in/out
constexpr float kEnterTravel = 16;  // logical px a panel rises/sinks while it fades in/out

}  // namespace halo::ui
