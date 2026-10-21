# Halo settings: the contract

One file, `%LOCALAPPDATA%\Halo\settings.ini` (UTF-8, `key = value`, `[section]` headers, `;` comments).
**Halo Preferences** (`halo-settings.exe`, the dashboard) writes it; **Halo** (`halo.exe`) watches it and
applies every change live, without a restart. A missing file, section or key means the default below. A value
out of range is clamped and a value Halo can't parse is ignored (the default is used instead), so a
hand-edited file can never break the shell.

Halo Preferences writes the whole file atomically: it writes `settings.ini.tmp`, then calls
`MoveFileEx(..., MOVEFILE_REPLACE_EXISTING)`. Halo re-reads the file when its directory reports a change,
debounced by 150 ms.

## Types

| Type    | Format                                                            |
|---------|-------------------------------------------------------------------|
| bool    | `on` / `off`                                                      |
| int     | decimal, clamped to the range                                     |
| color   | `#RRGGBB`                                                         |
| effect  | `rise` · `bounce` · `swipe` · `fade` · `swoop` · `none`           |
| theme   | `system` · `dark` · `light`                                       |

## Effects (open/close of a surface)

| effect | Open | Close |
|---|---|---|
| `rise`   | rises `travel` px from its edge with the reveal spring and fades in (the current look) | sinks back and fades |
| `bounce` | rises from further away (2× travel) on a looser spring (damping 0.5), so it overshoots and settles | as rise |
| `swipe`  | slides in sideways from the screen's right edge (travel along x) and fades | slides back out to the right |
| `fade`   | opacity only, no movement | fade out |
| `swoop`  | grows from 92% scale, anchored at the corner it opens from, while rising and fading in | shrinks and fades |
| `none`   | appears instantly | disappears instantly |

`speed` scales every duration and spring period of that surface (`100` = as designed, `50` = twice as
slow, `200` = twice as fast). `motion` scales distances and overshoot (`0` = no travel, so every effect except
`swoop` becomes a fade; `100` = as designed; `200` = twice the travel).

## Keys

### `[general]`
| key | type | default | range | meaning |
|---|---|---|---|---|
| `theme` | theme | `system` | | glass and text follow Windows, or are forced dark/light |
| `accent` | color | `#14A394` | | the "on" colour for toggles, sliders and focus everywhere |
| `reduce_motion` | bool | `off` | | every effect behaves like `fade`, hover magnification off |

### `[taskbar]` (the dock)
| key | type | default | range | meaning |
|---|---|---|---|---|
| `effect` | effect | `rise` | | how the dock reveals when the pointer reaches the bottom edge |
| `speed` | int | `100` | 50–200 | |
| `motion` | int | `100` | 0–200 | |
| `opacity` | int | `66` | 20–100 | glass tint strength, % |
| `tint` | color | `#18191E` (dark) / `#F8F8FB` (light) | | glass tint colour; empty = theme default |
| `blur` | int | `40` | 0–80 | reserved: Windows' host backdrop has a fixed blur, so Halo doesn't apply this yet (not shown in Halo Preferences) |
| `icon_size` | int | `48` | 32–72 | resting icon size, px |
| `magnification` | int | `150` | 100–250 | hovered icon size, % of `icon_size`; 100 = off |
| `autohide` | bool | `on` | | hide when a window touches the dock |
| `hide_delay` | int | `500` | 0–2000 | ms the pointer must be away before it hides |
| `bounce_on_launch` | bool | `on` | | the launching icon bounces |
| `running_dots` | bool | `on` | | dots under running apps |

### `[controlcenter]`
| key | type | default | range | meaning |
|---|---|---|---|---|
| `effect` | effect | `rise` | | |
| `speed` | int | `100` | 50–200 | |
| `motion` | int | `100` | 0–200 | |
| `opacity` | int | `66` | 20–100 | |
| `tint` | color | (theme) | | |
| `blur` | int | `40` | 0–80 | |
| `hot_corner` | bool | `on` | | opens when the pointer rests in the bottom-right corner |
| `corner_width` | int | `20` | 5–50 | width of the corner zone, % of the screen width |
| `corner_delay` | int | `120` | 0–600 | ms the pointer must rest there |
| `close_on_leave` | bool | `on` | | closes when the pointer leaves (if it was opened by the corner) |
| `show_media` | bool | `on` | | the Now Playing tile |
| `show_sliders` | bool | `on` | | the Display and Sound sliders |

### `[start]` (the launcher)
| key | type | default | range | meaning |
|---|---|---|---|---|
| `effect` | effect | `rise` | | |
| `speed` | int | `100` | 50–200 | |
| `motion` | int | `100` | 0–200 | |
| `opacity` | int | `66` | 20–100 | |
| `tint` | color | (theme) | | |
| `blur` | int | `40` | 0–80 | |
| `recent_apps` | int | `6` | 0–6 | apps in the recent row; 0 hides the row |
| `recent_files` | int | `12` | 0–12 | recent files (two rows of six); 0 hides them |
| `dim_desktop` | bool | `off` | | dim the screen behind Start |

## Live stats (Halo Preferences only)

Halo Preferences samples `halo.exe` and `guardian.exe` once a second:
- **CPU**: process time delta ÷ wall time ÷ logical cores, %.
- **RAM**: private bytes and working set, MB.
- **GPU**: the sum of the PDH counter `\GPU Engine(pid_<pid>_*)\Utilization Percentage` for those processes, %.

It also shows Desktop Window Manager's GPU, labelled "Desktop Window Manager (shared)": Halo's animations
run on the compositor, so most of their GPU cost lands in `dwm.exe`, not in `halo.exe`. These are
measurements, not estimates; where a counter is unavailable the dashboard says "n/a".
