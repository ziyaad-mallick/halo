// Screen-edge gestures on the primary monitor, for the Control Center and the Notification Center.
//
// Mouse (a WH_MOUSE_LL hook on its own thread; a few comparisons per move, no polling):
//   - resting on the right fifth of the bottom edge (corner_width) for corner_delay ms without clicking -> kBottom
//     (where the Windows taskbar keeps Quick Settings)
// Touch (two invisible strips, top edge and right edge, that only ever see touch and pen: whenever
// a real mouse comes near an edge they turn click-through, so they never steal a mouse click):
//   - a swipe down from the top edge  -> kTop
//   - a swipe left from the right edge -> kRight
//   (Windows claims right-edge swipes for its own panel unless the AllowEdgeSwipe policy is 0.)
//
// Each gesture posts `msg` to `target` with wParam = kBottom, kTop or kRight.
#pragma once
#include <windows.h>

namespace halo::edge {

constexpr WPARAM kTop = 1, kRight = 2, kBottom = 3;

bool Start(HINSTANCE inst, HWND target, UINT msg);  // UI thread
void Stop();
void Configure(bool hotCorner, int widthPct, int delayMs);  // [controlcenter] hot_corner, corner_width (% of the width), corner_delay
void OnDisplayChange();  // UI thread: re-measure the monitor and re-place the strips

}  // namespace halo::edge
