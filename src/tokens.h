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
constexpr float kPanelW = 360;    // a panel's width: Control Center, and the notifications attached above it
constexpr float kRound = 36;      // a round toggle inside a pill or tile
constexpr float kRoundAction = 44;  // the free-standing action buttons
constexpr float kPillH = 64;      // a connectivity pill (round toggle + two lines)
constexpr float kTileH = kPillH;  // the one tile height
constexpr float kButton = 36;     // a small button: the footer's, the media controls
constexpr float kRowH = 40;       // a page header or a list row that holds a title
constexpr float kListRowH = 36;   // a row in a device or output list
constexpr float kGroupGap = kS4, kInnerGap = kS2;  // between groups of controls; between controls in a group
constexpr float kRadiusHover = kRadiusSmall;       // the plate under a hovered or pressed control
constexpr float kThumbW = 3, kThumbMin = 24;       // a list's scroll thumb: width, shortest length
constexpr float kFocusRing = 2, kFocusGap = 2;     // keyboard focus: ring width, and the gap outside the control
constexpr float kHair = 1;                         // a 1 px stroke at any scale
constexpr float kSliderH = 22;    // the slider track as drawn (was 28: ~21 % shorter)
constexpr float kSliderHitH = kSliderH + 2 * kS1;  // its pointer target: 4 px taller on each side, so it is still easy to grab
constexpr float kMediaArt = 48;   // now-playing artwork
constexpr float kMediaH = kMediaArt + 2 * kS3;  // the now-playing module

// Notification Center: compact floating cards, a quiet heading, then a calendar widget.
constexpr float kNotificationWidth = kPanelW, kNotificationEdge = kS4;
constexpr float kNotificationPad = kS4, kNotificationGap = kS3;
constexpr float kNotificationRadius = kRadiusLarge;
constexpr float kNotificationInset = kS2, kNotificationPanelPad = kS3;
constexpr float kNotificationGlossEnd = 0.45f;
constexpr float kNotificationCalendarHead = kS4, kNotificationCalendarRow = 28;

// Motion.
constexpr int kFastMs = 90, kSlowMs = 280;  // fast was 120
constexpr int kHoverMs = 80;                // a highlight gliding between controls, and fading in/out
constexpr float kEnterTravel = 16;  // logical px a panel rises/sinks while it fades in/out
constexpr int kPulseMs = 260;       // the ring that springs out of a toggle when it is switched

}  // namespace halo::ui
