# Halo bug review, 2026-10-18

Scope: `git diff efdcab0 HEAD -- src/` on `feat/control-center-mvp` (HEAD 2edce46). Read-only review; nothing built or run.
Line numbers are for the committed HEAD files. The working tree has uncommitted edits to `control_center.cpp` (a `Park()` / `SizeVisuals()` keep-the-window-alive change, new sizes) and `theme.h`; those were not reviewed except where noted at the end.
Everything below is from reading code. Nothing was observed at runtime, so each claim about frame timing is marked UNVERIFIED where it depends on compositor/DWM behaviour.

Ranked most severe first.

---

## 1. Touch edge swipes open the Control Center as "by pointer", and it auto-closes about 330 ms later
Severity: logic / visible glitch. Confirmed from the code.

- `halo.cpp:1134`: `OpenPanel(true, w != halo::edge::kBottom);` passes `byPointer = true` for every edge gesture, including the touch swipes `kTop` and `kRight` (edge.cpp strips).
- `control_center.cpp:733-760` (`CheckPointer`): with `byPointer` true it skips the early return `if (!byPointer && !entered) return;`, then computes `in` from the panel rect, the sibling window, and the bottom-right hot zone only. A touch swipe from the top or right edge does not move the cursor into any of those, so `in` is false, `outSince` is set, and `Close()` runs once `now - outSince >= kLeaveMs` (280 ms, plus up to 50 ms tick).
- Scenario: swipe down from the top edge on a touch screen. The panel and the notification column slide in, then slide out again about 0.3 s later unless the stale cursor happens to be over the panel.
- Fix: `OpenPanel(w == halo::edge::kBottom, w != halo::edge::kBottom);` (only the mouse corner is "by pointer").

## 2. Slider drag redraws the whole panel and all 17 chip surfaces on every mouse move
Severity: visible stutter. Confirmed from the code; cost UNVERIFIED (no profile).

- `control_center.cpp:960-976` `DragTo` calls `Render()` on each `WM_MOUSEMOVE` during a drag.
- `Render()` (1109-1123) does `Layout()`, `ApplySize()`, a full `Paint`, then `SyncChips()`.
- `SyncChips()` (627-668) calls `Paint(dc, ..., &it, nullptr)` with a `BeginDraw/EndDraw` for every Scalable item even when the chip is reused unchanged (`c = std::move(old)` at 637, then the draw at 658-666 runs regardless). That is 17 small surface redraws per mouse move on top of the main surface.
- `kMsgAudio` (381), media events and brightness (388) go through the same path.
- This is the opposite of the commit's stated goal ("hovering never redraws anything").
- Fix: in `SyncChips`, redraw a reused chip only when its drawn state changed. Cheapest form: keep a small signature per chip (on/off state, label text, `media.State().playing/can*`, `micMuted`, `mode`) and skip the draw when equal. Also skip `SyncChips()` entirely from `DragTo` (sliders are not chips): give `Render` a `bool chips = true` argument.

## 3. Launcher `Refresh()` does file-system and shell work on the UI thread 420 ms after open
Severity: visible hang while typing. Confirmed that it is on the UI thread; duration UNVERIFIED.

- `launcher.cpp:1041` `SetTimer(..., kRefreshTimer, 420)`, handler at ~1506-1509 calls `Refresh()` (1162-1177), which calls `Prefetch()` (1123) -> `LoadFiles()` (355-399).
- `LoadFiles` loads up to 160 `.lnk` files through `IShellLink`/`IPersistFile::Load`, then `GetFileAttributesW(target)` per target, then `LoadJumboIcon` for up to 60 files. Only `\\server\share` paths are skipped (`target[0] == L'\\' && target[1] == L'\\'`). A recent file on a mapped drive letter whose share is offline, a sleeping USB disk or a cold disk can block `GetFileAttributesW` for seconds.
- Scenario: tap Win, start typing at once. At about 420 ms the UI thread stalls inside `Refresh`, so keystrokes and caret/results freeze; with a dead mapped drive it hangs for seconds. The compositor animations keep running, so it looks like a frozen search box.
- Fix: do `LoadFiles`/icon loading on a worker thread (the way `StartLate` does in control_center.cpp) and post the result; or at least skip targets whose drive is not `DRIVE_FIXED` (`GetDriveTypeW`) and cap the `looked` loop by time. Only run `Refresh` when the Recent folder's mtime changed.

## 4. Launcher (and the notification column) show the window with the panel at rest before the slide animation starts
Severity: visible glitch (intermittent). UNVERIFIED whether DWM presents a frame in the gap.

- `launcher.cpp:1036-1039`: `m_panel.Offset({0, home, 0}); Place();` then `motion::In(...)`. `Place()` ends with `SetWindowPos(... SWP_NOACTIVATE | SWP_SHOWWINDOW)` (line 1098), so the window becomes visible with the committed tree at its rest position, and only afterwards does `motion::In` start the spring from rest+travel.
- `notifications.cpp:1376-1378`: `ShowWindow(...)` and then `motion::In(...)`, the same order.
- `control_center.cpp:467-471` tries to guard this with `pill.Opacity(0)` before `ShowWindow`, but `motion::In` calls `detail::Settle(v)` which immediately sets `Opacity(1)` (motion.h `Settle`). The guard only works because both calls land in one commit, which is not guaranteed.
- Scenario: the first frame after Win-tap shows the full panel in place, then it jumps down and springs up.
- Fix: make `motion::In` set `v.Offset(rest + travel)` itself before `StartAnimation`, and call it before the window is shown (move the `motion::In` calls above `Place()` / `ShowWindow`; in the launcher split `Place()` so the show happens after). Drop the dead `pill.Opacity(0)`.

## 5. Growing the Control Center moves the window and resizes the visuals through two different pipelines
Severity: visible glitch (one frame jump when expanding a section). UNVERIFIED frame alignment.

- `control_center.cpp:1083-1098` `ApplySize`: `SetWindowPos(hwnd, nullptr, rect.left - pad, rect.top - pad, ...)` moves the window up by the growth, then `pill.Size(...)`, `clip.Size(...)`, `surf.Resize` change composition state.
- The window move is applied by DWM immediately, the composition changes at the next commit. For a frame the old-size pill is drawn from the new, higher window top, so the panel's bottom edge sits `delta` px too high (delta = 54 px for the power row, about 100+ px for a Wi-Fi list).
- It also makes the notification column jump: `onResized` -> `SetAnchor` -> `Update(false)` -> `Place()` runs another `SetWindowPos` (notifications.cpp:1291-1308), including mid-slide when the resize comes from `Settle()` at 330 ms while the notification spring (started about 40 ms later) is still running.
- Fix options: keep the window at a fixed maximal height (bottom-anchored) and move only `pill.Offset.Y`/sizes in the compositor so a resize is composition-only; or at least set `pill.Offset.Y = pad + delta` in the same call as `SetWindowPos`, then animate it back to `pad`, so the interim frame matches the old picture.

## 6. After a click that changes the layout, hover and the lifted chip stay on the old item
Severity: visible glitch. Confirmed from the code.

- The panel is bottom-anchored (`ApplySize` sets `rect.top = rect.bottom - hPx`) but content is laid out from the top, so any growth moves every existing item up on screen by the growth. Example: click Power -> `powerOpen = true` -> `Render()` -> window grows 54 px upward; the pointer is now over a different control.
- `control_center.cpp:378`: after the click, `HoverChip(ChipOf(hover.id != Id::None ? &hover : nullptr))` uses the stale `hover` (still the Power item, which moved away from the pointer). The Power chip lifts and leans toward a pointer that is no longer on it; the hover plate (`hv`) stays on a moved tile. It corrects itself only on the next mouse move.
- Fix: after any `Render()` that changed `height`, re-hit-test at the real cursor (`GetCursorPos` + `ScreenToClient`, convert as in `PanelProc`) and update `hover`, `SetHover`, `HoverChip`.

## 7. Launcher: `m_sel` can be out of range after `Refresh()`, indexing `m_resultRows` out of bounds
Severity: crash (very narrow window).

- `launcher.cpp:1180-1200` `RunSearch` rebuilds `m_results` but does not clamp `m_sel`. `Refresh()` (1171) calls `RunSearch()` then `Relayout()` -> `Build()` -> `BuildBody()` (812): `Highlight(&m_resultRows[m_sel], 12, false)` guarded only by `!m_resultRows.empty()`.
- Scenario (needs the Recent folder to change in the first 420 ms, user already typed and arrowed down): `m_sel` = 5, new result list has 3 entries, `m_resultRows[5]` reads past the vector. If the Power menu is open, `Relayout()` returns early (1150) and `m_results`/`m_resultRows` sizes diverge, so `OnKey` (1250-1251) and `OnHover` (1357) index with a clamp against the new `m_results.size()` but into the old `m_resultRows`.
- Fix: at the end of `RunSearch`, `m_sel = std::clamp(m_sel, 0, std::max(0, (int)m_results.size() - 1));`, and in `Refresh` only call `RunSearch()` when `m_query` is empty or after rebuilding the rows.

## 8. SyncChips remaps `hotChip` to a rebuilt chip that never gets its hover lift
Severity: visible glitch (minor).

- `control_center.cpp:674-678` restores `hotChip` by item identity after `chips = std::move(next)`. If the hovered control's rect changed (a section above it expanded/collapsed), `next` holds a brand-new chip visual with no Scale/Offset animation, while `hotChip` points to it.
- `HoverChip` (689) then returns at `if (i == hotChip) return;`, so the lift/lean is not applied until the pointer leaves and returns.
- Fix: after the remap, if the chip is hot and was newly created, call the grow/lean part of `HoverChip` unconditionally (set `hotChip = -1` and then `HoverChip(idx)`).

## 9. `Settle()` from `WM_LBUTTONDOWN` can re-layout under the click
Severity: logic (rare). UNVERIFIED how often `SyncAll` changes the layout.

- `control_center.cpp:329-331`: `Settle()` runs before `x/y` are hit-tested. `Settle` -> `SyncAll()` (speaker availability, `outs`, brightness support) -> `Render()` -> `Layout()` + `ApplySize()`. If the height changes (the Sound/Display tile appears or disappears), the window moves up/down, but `x, y` were computed from `lParam` in the old geometry, and `HitAt(x, y)` then runs against the new items: a different control than the one under the finger gets pressed.
- Fix: hit-test first, `Settle()` after, or recompute `x,y` from `GetCursorPos` after `Settle()`.

## 10. Media (GSMTC): stale-overwrite race and a dangling `this` at shutdown
Severity: logic (minor) / crash at exit only (UNVERIFIED).

- `services.cpp:683-731` `PullAsync`: every media event starts a new coroutine; `gen` only changes in `Stop()`. Two pulls in flight (a track change fires `MediaPropertiesChanged` and `PlaybackInfoChanged` back to back; art decode is slow) can complete out of order, and the older one writes `m_state = std::move(next)` last, so the tile shows the previous track until the next event.
- Fix: bump a per-pull sequence number (`unsigned seq = ++m_seq;`) and drop results with `seq != m_seq` after each `co_await`.
- `control_center.cpp:1566-1570`: `~ControlCenter` calls `media.Stop()` (bumps `m_gen`) and then the `Impl` is freed. A coroutine suspended in `TryGetMediaPropertiesAsync` / `OpenReadAsync` resumes later and reads `m_gen` through a freed `this`. Only matters if the message loop keeps running after `m_cc.reset()` (halo.cpp:1208, WM_CLOSE path). Fix: hold a `shared_ptr` state in the coroutine instead of `this`.
- Checked and fine: the `changed` callback runs on the UI thread (coroutines started from `HotThunk`/`Init` on the UI thread resume on the captured apartment), is guarded with `st == Open`, and `Render()` returns early while settling or closing, so it cannot run mid-`Teardown`.

## 11. `motion::Out` snaps to rest when closed mid-spring
Severity: visible glitch (minor).

- `motion.h` `Out`: `slide.InsertKeyFrame(0.0f, rest)` forces the animation to start at the rest position, so closing while the opening spring is still moving (a quick double Win tap, Esc right after open, an accidental hot-corner flick that closes at about 330 ms) jumps the panel to rest first, then plays the anticipation nudge.
- Fix: omit the 0.0 keyframe so the animation starts from the current value, and key the nudge at 0.25 relative to `rest`.

## 12. Notification column is clipped or off-screen when the Control Center is tall
Severity: visible glitch on small screens. UNVERIFIED on the target laptop.

- `notifications.cpp:1337-1339`: `availH = std::max(120.0f, (bottom - mi.rcMonitor.top) / s - kAttachMargin)`. When the room above the Control Center is under 120 logical px, the floor of 120 makes `Place()` put the window at `yy = anchorRc.top - gap - hPx + pad` (line 1304), which is above the monitor top. On a 1080p panel at 150% (720 logical px) with the CC at about 520 px the space above is about 180 px, which is less than the compact calendar height (about 182-206 px), so the bottom of the column is cut at the window edge; with an expanded Wi-Fi list (CC capped at 704 px) the column is entirely off-screen.
- Fix: if `availH` is below the compact calendar height, skip attaching (do not open the column) or open it standalone on the right.

## 13. The Control Center window overhangs the screen bottom by 8 px (and swallows clicks on a monitor below)
Severity: minor.

- `control_center.cpp:457-463`: panel bottom = `mon.bottom - 8*s`, window bottom = panel bottom + `pad` (16*s), so the window extends `8*s` px below the primary monitor. With a second monitor stacked below, that strip is a transparent hit-testable window over the other monitor, and the slide-in/out panel is visible in it (the window clip is at `mon.bottom + 8*s`, not at the monitor edge).
- Fix: clip the visual with the monitor edge (`root.Clip`) or reduce the bottom pad to `kMargin`.

## 14. Teardown from `Open()`'s catch re-enters `Close()` and reports `onHidden` twice
Severity: minor.

- `control_center.cpp:487-490`: any `hresult_error` after `st = St::Open` (e.g. `Render()` -> `SyncChips` -> `CreateDrawingSurface`) calls `Teardown()` with `st == Open`. `DestroyWindow(hwnd)` (541) delivers `WM_ACTIVATE(WA_INACTIVE)` while `st` is still `Open`, so `PanelProc` runs `Close()` (starts `motion::Out`, `SetTimer`, fires `onHidden`) on a window being destroyed; then `Teardown` fires `onHidden` again (552) although `onShown` was never called.
- Fix: set `st = St::Closed` before `DestroyWindow`.
- Also: `kMsgLate` payloads posted to a window that is destroyed in the gap are freed only if `PostMessageW` fails (800); an in-flight message to a destroyed window is lost with its `Late` (tiny leak).

## 15. Hot-corner: no fullscreen/context check, and a stale Esc watch flag
Severity: logic (minor). UNVERIFIED.

- `edge.cpp:44-76`: the 120 ms dwell in the bottom-right fifth fires `kBottom` regardless of what is in the foreground. In a fullscreen game/video where the pointer is pinned to the bottom edge, or while the dock is deliberately hidden for a fullscreen app, the Control Center pops up over it. The previous design had the same hazard in the top-right corner but with a much smaller target (the fifth of the whole bottom edge is about 384 px wide).
- `halo.cpp:330-333`: the dock menu's `closed` handler leaves `WatchEscape` true when the launcher is open, and nothing resets it when the launcher closes. The next launcher open then also receives Esc through the hook's `EscapeMessage` path, so Esc closes the panel outright (`halo.cpp` `m_launcher->Hide()`) instead of first clearing the typed query (`launcher.cpp` `OnKey` Esc handling).
- Fix: gate the corner on `intellihide` fullscreen state (the `kMsgIntellihideChanged` lparam already carries it); have `Launcher::Hide`'s host `closed` callback call `WatchEscape(false)` when no menu is open.

---

## Checked and found sound

- `Open -> Close -> Open` while `Closing`: `Open()` calls `Teardown()` first (control_center.cpp:431), `Close()` kills the leave/settle timers and clears `settling/held/needSync` (506-509); a stale `kTimerClose` cannot fire because the old window is destroyed.
- `Render()` during `Closing` returns at `st != Open`; while `settling` it only sets `held`, and `Settle()` is idempotent (the `settling` guard) whether it comes from the timer or from a click.
- `Paint` skipping Scalable items while `chipLayer` is null is not a problem on open: `BuildVisuals` creates `chipLayer` before the first `Render()`. (It would leave the buttons invisible if `SyncChips` threw mid-way, but it is caught by `PanelThunk`/`Open`.)
- The lean expression animation holds `ptr`; `Teardown` nulls `ptr` and releases the tree together, so nothing references it afterwards.
- Notification travel math: `Travel()` = `(winH + kPad) * s` added to `rest.y = kPad * s` starts the panel exactly at the window's bottom edge, so it is fully clipped at the start; `winH` is set by `Update(true)`/`Place()` before `motion::In` is called.
- `availH` in `Layout()` cannot go negative (`max(200, ...)`); the notification `availH` is floored at 120 (see 12).
- Launcher `WM_CHAR`: control characters are rejected, a surrogate pair backspaces as one, the leading space is dropped, and the result list is capped at `kMaxResults = 8` rows (276 logical px) inside the 286 px body. The only gap is that `kMaxQuery` can split a surrogate pair, leaving a lone high surrogate (cosmetic).
- Launcher `Hide()`/`Close()`: `gen` correctly invalidates the fade-out after a reopen. The scoped batch relies on `Completed` firing; there is no timer fallback (the CC and notifications use a timer), so if the batch never completes the window stays up until the next `Show()` (UNVERIFIED, low).

## Uncommitted working-tree changes (not part of the diff above)

`control_center.cpp` currently contains a `Park()` path that keeps the window and visuals between opens. Notes from reading that diff only: `Park()` does not kill `kTimerClock` (harmless, it checks `st`); `Open()` calls `Render()` before the first show against a window that was last parked at the slide-out end offset, which is fine only because `motion::In` overwrites `Offset`; on a DPI change `Teardown()` runs from inside `Open()` before `st` is set, which is correct. The new `kW = 356` and taller sections make finding 12 worse on 150% 1080p displays.
