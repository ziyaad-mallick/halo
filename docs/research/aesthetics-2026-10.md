# Halo: making it more aesthetic (research, 2026-10-18)

Read-only research pass. Code read: src/theme.h, motion.h, effects.h, control_center.cpp (constants, glass stack, tile/icon painters, fonts), launcher.cpp and notifications.cpp (constants, fonts). Nothing was built or run, so every "exact value to try" is a starting point to A/B by eye, not a measured result. Tag legend: FETCHED = I read the page this session; SNIPPET = only seen in a search-result summary; UNVERIFIED = from memory or inference.

## (a) Executive summary

1. The glass recipe is already the right shape (backdrop, tint, grain, gradient rim, shadow). The gains now are in details, not a rebuild.
2. Biggest cheap win is typography: Halo's secondary text is 11.5px and 11px, below Microsoft's stated 12px regular minimum. Move to the Windows ramp (12/14/20, Small/Text/Display optical sizes), Regular and Semibold only.
3. Second is geometry: make radii concentric and snap to an 8px grid (panel 28, inset 12, tile 16, gap 8). Apple's own talk names concentricity as the rule for nesting glass.
4. Third is a static "thickness" cue: an inner top-edge highlight on the panel rim and on dark tiles. Drawn once, zero per-frame cost. The honest approximation of Liquid Glass specular edges.
5. Pointer sheen is cheap because `ptr` ("P") already exists in control_center.cpp: one small radial-gradient sprite bound to it by an ExpressionAnimation. Do not use PointLight (see do-not list).
6. Backdrop saturation (the macOS vibrancy look) needs a custom Saturation effect node and a second pass over the host backdrop. Halo already measured an extra blur pass dropping frames, so measure first, and try the dock (small area) before panels.
7. Dark mode has shadow alpha 0 and blur 0 in theme.h, so dark panels have no separation from bright windows behind them. Re-using the light-mode shadow path is a one-constant change.
8. Liquid Glass lensing/refraction is NOT reproducible with the Composition effect list I could verify. Fake the cues (rim, highlight, tint) instead of chasing it.
9. Honour "Transparency effects off" and Battery Saver with a solid fallback (#2C2C2C dark, #F9F9F9 light, per WinUI): polish and accessibility in one.
10. Apple's exact Control Center grid, radii and type sizes could not be fetched (developer.apple.com HIG pages returned ECONNRESET or empty bodies); those numbers are marked UNVERIFIED and I lean on what I could fetch.

## What Halo has today (from code)

| Item | Value | Where |
|---|---|---|
| CC panel | width 340, inset 14, gap 10, radius 24, tile radius 18 | control_center.cpp:54 |
| CC heights | pill 60, row 68, slider 52, media 88, head 76, action 60, power 48 | control_center.cpp:55 |
| Launcher | pad 24, radius 24, search h 40, icon 44, row 64, button h 32 | launcher.cpp:77-95 |
| Notifications | width 360, pad 14, icon 32, radius 18, gap 10 | notifications.cpp:66-69 |
| Fonts (CC) | Text 13 SB title, Text 11.5 R sub, Text 11 R caption, Text 12.5 SB button, Display 34 SB clock, Fluent Icons 18/14 | control_center.cpp:1376-1385 |
| Dark glass | blur 40 (DWM), tint #16161A alpha 0x80 (50%), noise 2.5%, rim white 0x70/0x18/0x0A/0x26, shadow alpha 0 blur 0 | theme.h dark palette |
| Light glass | tint #F8F8FB alpha 0xD6 (84%), noise 1.8%, edge hairline, shadow 0x2E blur 28 dy 8 | theme.h light palette |
| Dark ink | text .95, soft .85, dim .58, faint .26 (white alpha) | theme.h |
| Dark fills | tile 12% white, hover 20%, press 7%, off-toggle 17%, accent #0A85FF | theme.h |
| Hover | one gliding rounded rect, white 15% (0x26) in dark, 7% black in light | control_center.cpp ~1480 |
| Glass stack | HostBackdropBrush, tint, grain, D2D content surface, hover, fx, gradient rim | control_center.cpp:1442-1511 |
| Pointer plumbing | `ptr` PropertySet with Vector2 "P", default (-1000,-1000), used for chip lean | control_center.cpp:180, 1514 |
| Effects available | custom `GaussianBlurEffect` only (effects.h); no Saturation/ColorMatrix node implemented | effects.h |

## (b) Ranked recommendations

Ranking is visual payoff divided by effort, with fps risk as a veto. "Touches" names the file and constant.

### 1. Type ramp fix (effort S, fps risk none)
- Why: Windows 11 minimums are 14px Semibold or 12px Regular. Ramp: Caption 12/16 (Small), Body 14/20, Body Strong 14/20 semibold, Subtitle 20/28 Display semibold, Title 28/36 Display semibold. Regular and Semibold only (no Bold, no Italic), sentence case. FETCHED: https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/typography
- Values: fSub/fSubR/fCap: "Segoe UI Variable Small" 12 Regular. fTitle/fBtn/fBatt: "Segoe UI Variable Text" 14 Semibold. Clock keeps "Display" 34; A/B Semibold against SemiLight (350) for a calmer, Apple-like numeral. Launcher fRow 13 to 14, fLabel 11 SB to 12 SB.
- Touches: control_center.cpp:1376-1385; launcher.cpp:543, 831-954; notifications.cpp fonts.

### 2. Concentric radii and 8px grid (effort S-M, fps risk none)
- Why: Apple says glass controls nest into rounded corners "maintaining concentricity" (FETCHED transcript, no formula given): https://developer.apple.com/videos/play/wwdc2025/219/ . Inner radius = outer radius minus inset is the standard rule. The 8px grid is my own consistency heuristic, not a sourced spec. Fluent treats pill shapes as their own rounded-corner category: https://fluent2.microsoft.design/shapes (FETCHED).
- Values: kRadius 24 to 28, kInset 14 to 12, kTileR 18 to 16 (28-12), kGap 10 to 8. Heights on 8px multiples: pill 60 to 56, row 68 to 64, slider 52 to 56, head 76 to 72, act 60 to 56. Width 340 to 344. Notification card radius 18 to 16 with pad 12. Search bar radius = height/2.
- Touches: control_center.cpp:54-55; launcher.cpp:77-95 and kPanelRadius; notifications.cpp:68. Re-check hit-test rects and kMaxH after changing.

### 3. Inner top-edge "specular" line on the panel rim (effort S, fps risk none)
- Why: Liquid Glass specular highlights "define its silhouette", and large glass reads thicker (WWDC25 page above, FETCHED). Apple newsroom: Dock and widgets are "multiple Liquid Glass layers with specular highlights": https://www.apple.com/newsroom/2025/06/apple-introduces-a-delightful-and-elegant-new-software-design/ (FETCHED). Seelen's Neo Glass sells the same cue (blur, glow, edge highlight): https://seelen.io/resources/_XR-flD3RsyoicNMxNt_vg/neo-glass (FETCHED).
- Values: add a second stroke shape inside the existing rimVisual: same geometry inset one extra pixel, 1px, gradient white alpha 0x38 at y=0 to 0x00 at y=0.25, nothing below. Dark only; light uses white 0x80 to 0. Keep the existing rim.
- Touches: control_center.cpp:1488-1511, plus the same recipe in the dock pill, launcher and notifications. Static ShapeVisual, no animation.

### 4. Top highlight and bottom shade on dark tiles (effort S, fps risk none)
- Why: same reasoning as 3. The light tile already has a 1px white 55% inner stroke; the dark tile only has a 9% white stroke over a vertical gradient.
- Values: in the dark `tile()` branch, add a top 1px line white 0.16 fading to 0 over the first 35% of height (reuse the `lg` gradient brush with start/end r.top to r.top+0.35h) and a 1px bottom line black 0.18.
- Touches: control_center.cpp:1146-1153. One-time D2D draw.

### 5. Dark-mode shadow (effort S, fps risk low)
- Why: dark palette has shadow alpha 0 and blur 0, so MakeShadow returns null (control_center.cpp:115) and dark panels do not separate from bright windows behind. Fluent mentions low and high elevation ramps, but I only saw that in a third-party kit summary (SNIPPET, https://pub.dev/packages/fluent2_kit); the Fluent shapes page had no elevation values.
- Values: dark shadow {0x66,0,0,0}, blur 40, dy 14 for panels; the dock keeps 0x40/24/8. This reuses the light-mode path, which already ships.
- Touches: theme.h dark Palette (shadow, shadowBlur, shadowY).

### 6. Pointer-following sheen (effort M, fps risk low-medium)
- Why: Liquid Glass highlights "respond to movement" (WWDC25, FETCHED). PointLight is the textbook route but has hard limits (see do-not 3; limits FETCHED at https://learn.microsoft.com/en-us/windows/uwp/composition/composition-lighting ). A radial-gradient sprite is the safe equivalent.
- Values: one SpriteVisual 240x240 px with a CompositionRadialGradientBrush, white alpha 0x14 (dark) / 0x30 (light) at the centre to 0x00 at the edge, inside `inner` below `content`. Offset = ExpressionAnimation `ptr.P - Vector2(120,120)`. Because P starts at (-1000,-1000) it sits off-panel until the pointer arrives. Updated only by the property-set write Halo already does on pointer events. No opacity animation.
- Touches: control_center.cpp near 1514 where `ptr` is made; optionally launcher.cpp. A small alpha sprite is not a full-panel layer, but check with tools/framegaps.cpp.

### 7. Honour Transparency off and Battery Saver (effort S, fps risk none, reduces cost)
- Why: Microsoft falls back to solid in those cases and says not to put accent-coloured text on acrylic. FETCHED: https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic . Fallback colours FETCHED from the WinUI source: https://raw.githubusercontent.com/microsoft/microsoft-ui-xaml/6aed8d97fdecfe9b19d70c36bd1dacd9c6add7c1/dev/Materials/Acrylic/AcrylicBrush_19h1_themeresources.xaml
- Values: if the Personalize EnableTransparency registry value is 0, skip HostBackdrop and fill solid #2C2C2C (dark) / #F9F9F9 (light) at alpha 0xF2. The registry value name is from memory: UNVERIFIED, confirm before coding.
- Touches: theme.h (new flag); glass build at control_center.cpp:1444 and the launcher/notification equivalents.

### 8. Dark-glass legibility floor (effort S, fps risk none)
- Why: WinUI dark acrylic default is tint #2C2C2C at TintOpacity 0.15 with base #202020 (FETCHED file in 7). Halo is darker (#16161A) and heavier (50%). TintLuminosityOpacity is set in code-behind, so the true Windows look is UNVERIFIED. Microsoft warns that tint/opacity changes affect legibility (acrylic page).
- Values: A/B three tints: #16161A@0x80 (today), #1B1B20@0x99 (60%), #202020@0xA6 (65%). Pick the lowest alpha where `dim` text stays readable over a bright wallpaper. Also try dim 0.58 to 0.64.
- Touches: theme.h dark.tint, dark.dim.

### 9. Toggle polish: one accent, glossier "on" (effort S, fps risk none)
- Why: WWDC25 says to tint selectively for primary elements, since tinting everything makes nothing stand out, and glyphs flip to maximise contrast (FETCHED). Halo already uses a single accent with a white glyph.
- Values: on state = accent with a vertical gradient from +8% lightness (top) to base (bottom) plus the 1px top highlight (white 0x40) from rec 4. Off state keeps 17% white; glyph alpha .95 to 1.0. Keep ONE accent; no per-function colours. Optional: read the system accent via UISettings (Seelen follows the system accent, FETCHED) with #0A85FF as fallback.
- Touches: control_center.cpp:1171-1206 (`icon` lambda); theme.h accent.

### 10. Inset search field in the launcher (effort S-M, fps risk none)
- Why: Raycast-style launchers stay polished through restraint: material window, text hierarchy through foreground levels, hint badges at 20% secondary, 12pt window radius. This comes from illustrative sample code, not an official spec: https://blakecrosley.com/guides/design/raycast (FETCHED).
- Values: search field fill black 0.18 (dark) / black 0.05 (light) so it reads recessed against raised tiles, with a 1px inner top line black 0.25. Selected result row = accent at 0.20 fill, not full accent. Hint chips: white 0.20 fill, radius 4-6, 12px text.
- Touches: launcher.cpp search bar paint (kSearchH 40 region) and result-row selection.

### 11. Hover and press ring (effort S, fps risk none)
- Why: the existing hover glide is good; a 1px rim makes it match the glass language. No external source; design judgement.
- Values: `hs` shape StrokeBrush white 0.14 (dark), black 0.06 (light), 1px. Press fill 0x1A to 0x0F.
- Touches: control_center.cpp ~1480 (hv/hs).

### 12. Staggered entrance for chips and cards (effort M, fps risk low)
- Why: small per-control animations are allowed, and Halo already separates chips (`chipLayer`) and notification cards. Offset-only springs, no panel opacity. Apple gives no timing numbers: timing UNVERIFIED.
- Values: delay each chip's offset spring 18ms x index (cap 5), travel 10px up, using the dock spring in motion.h (damping .72, period 55ms).
- Touches: control_center.cpp chipLayer open path; notifications.cpp card insert.

### 13. Adaptive tint for bright wallpapers (effort M, fps risk low)
- Why: Liquid Glass tint adapts to what is behind it; large elements adapt but do not flip, small ones flip (WWDC25, FETCHED). Halo equivalent: sample once, never per frame.
- Values: on panel open, average luminance of the screen rect behind the panel (one BitBlt, or a cached wallpaper average recomputed on theme/wallpaper change). If L > 0.65 raise dark tint alpha by +0x1A; if L < 0.25 lower it by 0x10. Do not flip palettes.
- Touches: new helper in theme.h, read in each panel's open path. Must not run during the animation.

### 14. Backdrop saturation boost, the vibrancy look (effort M-L, fps risk HIGH)
- Why: Composition lists a Saturate effect and a Blend effect with 21 of 26 blend modes (FETCHED, https://learn.microsoft.com/en-us/windows/apps/develop/composition/composition-effects , WinAppSDK flavour; the same effects exist in Windows.UI.Composition, inference). Microsoft's acrylic recipe is background, blur, exclusion blend, tint, noise (FETCHED acrylic page); Halo skips the exclusion layer. Apple's exact vibrancy numbers: UNVERIFIED.
- Values: add a second IGraphicsEffect node like GaussianBlurEffect in effects.h (CLSID_D2D1Saturation, one float prop, source = HostBackdropBrush through an effect factory). Try Saturation 1.25, then 1.5. Try the dock first (small area), then CC.
- Touches: effects.h (new node) and the glass build in each surface. Halo already measured an extra Gaussian pass dropping frames over a moving backdrop, so gate on tools/framegaps.cpp before shipping.

### 15. Text hierarchy tokens (effort S, fps risk none)
- Why: three text levels on glass and nothing else; no accent text on glass (Microsoft rule, FETCHED acrylic page).
- Values: primary .95, secondary .64, tertiary .38. Notification title 14 SB, body 12 R at .64, timestamp 12 R at .45. Replace ad-hoc alphas with theme tokens.
- Touches: theme.h (add `tertiary`), notifications.cpp paint.

### 16. Softer dividers (effort S, fps risk none)
- Why: dividers at 12% white compete with tile edges. Design judgement.
- Values: line white 0.12 to 0.08 (dark); black 0.07 to 0.05 (light).
- Touches: theme.h `line`.

Order of work: 1, 3, 4, 5, 7, 8, 9, 11, 15, 16 are static or constant changes and fit one afternoon. Then 2 and 6. Item 14 last, and only if framegaps stays clean.

### What of Liquid Glass is reproducible

| Liquid Glass cue (source) | Reproducible? | How |
|---|---|---|
| Specular rim that defines the silhouette (WWDC25 FETCHED) | Yes | Rec 3 |
| Specular highlight that moves (WWDC25 FETCHED) | Partly | Rec 6: radial sheen bound to `ptr`; not geometry-aware |
| Tint adapting to content (WWDC25 FETCHED) | Partly | Rec 13: one-shot luminance sample |
| Lensing / refraction of the backdrop (WWDC25 FETCHED) | No (inference) | The verified Composition effect list has no displacement or lens node, and the host backdrop is not cheaply sampled into custom effects. A D2D DisplacementMap may exist but I did not verify it: UNVERIFIED |
| Adaptive shadow over text vs solid (WWDC25 FETCHED) | No | Use a fixed soft shadow (rec 5) |
| Regular vs Clear; Clear needs a dimming layer (WWDC25 FETCHED) | Policy only | Halo is "Regular"; never mix |
| Large glass does not flip light/dark, small does (WWDC25 FETCHED) | Policy only | Keep the palette fixed per surface |

## 2. Windows 11 / Fluent 2 deviations (dark palette)

All values FETCHED unless marked.
- WinUI dark acrylic default: tint #2C2C2C, TintOpacity 0.15, fallback #2C2C2C; "Base" #202020 at 0.0. Light default: #FCFCFC at 0.0, fallback #F9F9F9; light Base #F3F3F3. Source: AcrylicBrush_19h1_themeresources.xaml (URL in rec 7). Halo dark (#16161A at 50%) is darker and more opaque than the Windows default; Halo light (#F8F8FB at 84%) is far more opaque than the Windows light default (0% tint). Because the Windows luminosity layer is code-behind, I cannot say how closely they look alike: UNVERIFIED.
- Acrylic is for transient, light-dismiss surfaces and should not be layered or placed edge to edge: https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic . Halo's panels are transient, which fits. Keep the 10px gap between CC and the notification column so there is no seam.
- Fluent 2 radius tokens: Medium 4, Large 8, X-Large 12 (popovers); circle 50%; shapes under 32px drop to 2px; thin stroke 1px: https://fluent2.microsoft.design/shapes . Halo's 24-28px panels are deliberately more macOS-like than Fluent's 12px; that is a style choice, not an error. Windows 11 Quick Settings' own dimensions were not found: UNVERIFIED.
- Fluent 2 Material page confirmed to contain no numeric values: https://fluent2.microsoft.design/material
- Segoe UI Variable optical size is automatic from 8 to 36pt (FETCHED typography page). Using the "Small"/"Text"/"Display" family names in DirectWrite as the manual equivalent is UNVERIFIED; test that "Segoe UI Variable Small" resolves.
- Apple macOS text styles (Body 13/16, Headline 13 bold, Callout 12, Subheadline 11, Footnote 10, Title 3 15, Title 2 17): UNVERIFIED, from memory; the HIG Typography page returned no body. Closest mapping for a mac feel: Apple 13 maps to Windows 14 Text, Apple 11 to Windows 12 Small, Apple 22 to Windows 20 Display SB.

## 3. Reference products: what makes the polished ones look good

- Raycast: native material instead of a flat fill, restrained radii (12pt window, 8 HUD, 4 badge), text hierarchy through primary/secondary/tertiary levels rather than many sizes and colours, one system accent, and a speed target (50ms perceived latency). Source is an unofficial guide with illustrative code: https://blakecrosley.com/guides/design/raycast (FETCHED). Takeaway: fewer colours, more alpha levels.
- Seelen UI "Neo Glass" theme: dock, toolbar and popups share one glass language with blur, glow and edge highlight; toolbar split into separate floating islands; 17 widgets (quick settings, notifications, menus) styled together; follows system accent and light/dark: https://seelen.io/resources/_XR-flD3RsyoicNMxNt_vg/neo-glass (FETCHED). Takeaway: consistency across every surface beats any single effect, so land palette, radius and type changes in theme.h for all panels together.
- Hyprland blur defaults (SNIPPET only, page body not read): noise 0.0117, contrast 0.8916, brightness 0.8172 in older docs (1.0 in newer), vibrancy 0.1696, size 8, passes 1: https://wiki.hypr.land/Configuring/Basics/Variables/ (fetch returned navigation only). Takeaway: Linux ricing converges on about 1% noise plus a modest saturation lift, matching rec 14 and Halo's existing 1.8-2.5% grain.
- Alfred, Arc command bar, Start11/StartAllBack, ExplorerPatcher, Nilesoft, Rainmeter skins, eww/AGS configs: not researched in this pass (no fetched source). Any claim about them would be UNVERIFIED.

## 4. Cheap delight details (summary)

- Pointer sheen: rec 6. Inner top highlights: recs 3, 4. Glossy accent toggles: rec 9. Staggered chip entrance: rec 12.
- Icon sizing heuristic (my own, UNVERIFIED as a standard): glyph about 40-45% of the circle's diameter, so an 18px glyph suits a 40-44px circle.
- Monochrome until on is already implemented; keep it. Do not colour-code Wi-Fi/Bluetooth/Focus differently.
- Grain: keep 1.8-2.5%. Microsoft's acrylic noise value was not in the pages I fetched, so I do not quote one.

## (c) Do-not list

1. Do not animate Opacity or Scale on whole panels. Halo's own measurements (motion.h header) showed dropped frames; keep slide-only springs.
2. Do not add a second Gaussian over the host backdrop (control_center.cpp:1442 comment records the frame drops). Do not add the saturation pass to panels without a tools/framegaps.cpp run.
3. Do not use CompositionLight / PointLight / SpotLight / SceneLightingEffect for the sheen. FETCHED limits: a non-ambient light turns off existing lighting and untargeted items appear black, only two non-ambient lights are allowed, lights must sit above perspective transforms, and SceneLightingEffect produces no shadows (https://learn.microsoft.com/en-us/windows/uwp/composition/composition-lighting). Whether a light can light a HostBackdropBrush at all is UNVERIFIED. Use the radial-gradient sprite.
4. Do not run a timer, per-frame callback or CPU work to move highlights. Pointer events writing to `ptr.P` only.
5. Do not stack glass on glass (a blurred card over a blurred panel). Tiles stay flat fills. Microsoft: avoid layered acrylic and edge-to-edge panes because of seams (acrylic page, FETCHED).
6. Do not put accent-coloured text on glass (Microsoft rule, FETCHED). Accent is for fills only.
7. Do not use Bold or Italic (Microsoft: use Semibold; italic hurts readability, FETCHED). Nothing below 12px Regular or 14px Semibold. No ALL CAPS; Windows uses sentence case.
8. Do not tint every control a different colour (WWDC25: tinting everything makes nothing stand out).
9. Do not use a "Clear" glass look without a dimming layer, and never mix Regular and Clear (WWDC25).
10. Do not raise grain above about 3%; on dark glass it starts to look like dirt.
11. Do not animate shadow blur or rim gradients. They are static surfaces.
12. Do not auto-flip a whole panel between light and dark by wallpaper; large glass should adapt, not flip (WWDC25).
13. Do not write "Apple spec" in code comments for radii or sizes until verified; the HIG pages could not be read in this pass.
14. Do not grow panels or shadows past the window margin (motion.h kPad 16) without widening the window, or the spring overshoot and shadow will clip.

## (d) Sources

FETCHED (page body read):
- Windows type ramp and Segoe UI Variable: https://learn.microsoft.com/en-us/windows/apps/design/signature-experiences/typography
- Acrylic guidance: https://learn.microsoft.com/en-us/windows/apps/design/style/acrylic
- In-app acrylic custom brush properties: https://learn.microsoft.com/en-us/windows/apps/develop/ui/in-app-acrylic
- System backdrops (Mica, Mica Alt, Acrylic Base/Thin; no numbers): https://learn.microsoft.com/en-us/windows/apps/develop/ui/system-backdrops
- Composition lighting: https://learn.microsoft.com/en-us/windows/uwp/composition/composition-lighting
- Composition effects list: https://learn.microsoft.com/en-us/windows/uwp/composition/composition-effects (page canonical: https://learn.microsoft.com/en-us/windows/apps/develop/composition/composition-effects)
- WinUI acrylic default tint values: https://raw.githubusercontent.com/microsoft/microsoft-ui-xaml/6aed8d97fdecfe9b19d70c36bd1dacd9c6add7c1/dev/Materials/Acrylic/AcrylicBrush_19h1_themeresources.xaml
- Fluent 2 shapes: https://fluent2.microsoft.design/shapes
- Fluent 2 material (no numbers): https://fluent2.microsoft.design/material
- WWDC25 Session 219 transcript (Liquid Glass): https://developer.apple.com/videos/play/wwdc2025/219/
- Apple newsroom, new software design: https://www.apple.com/newsroom/2025/06/apple-introduces-a-delightful-and-elegant-new-software-design/
- Raycast design guide (unofficial): https://blakecrosley.com/guides/design/raycast
- Seelen Neo Glass theme: https://seelen.io/resources/_XR-flD3RsyoicNMxNt_vg/neo-glass

SNIPPET only (search summary, not read in full):
- Hyprland blur defaults: https://wiki.hypr.land/Configuring/Basics/Variables/
- Fluent 2 elevation tokens via a third-party kit: https://pub.dev/packages/fluent2_kit
- Apple Icon Composer / Liquid Glass overview: https://developer.apple.com/icon-composer/

FAILED to fetch, so not used as evidence: developer.apple.com HIG Materials (ECONNRESET), HIG Typography and the Liquid Glass documentation page (empty bodies), WinUI Common_themeresources_any.xaml (404), wiki.hypr.land Configuring/Variables (404).

UNVERIFIED items collected: Apple Control Center grid, radii and spacing; Apple vibrancy levels; Apple type sizes; the EnableTransparency registry value name; Windows 11 Quick Settings dimensions; the luminosity layer of Windows acrylic; whether a D2D DisplacementMap works in Composition; whether DirectWrite resolves "Segoe UI Variable Small" by family name; any claim about Alfred, Arc, Start11, StartAllBack, ExplorerPatcher, Nilesoft, Rainmeter, eww, AGS.
