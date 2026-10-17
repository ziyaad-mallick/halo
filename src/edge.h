// Screen-edge gestures on the primary monitor, for the Control Center and the Notification Center.
//
// Mouse (a WH_MOUSE_LL hook on its own thread; a few comparisons per move, no polling):
//   - resting in the top-right corner for kCornerDwellMs without clicking -> kTop
//   - pushing on into the right edge after the pointer has reached it    -> kRight
// Touch (two invisible strips, top edge and right edge, that only ever see touch and pen: whenever
// a real mouse comes near an edge they turn click-through, so they never steal a mouse click):
//   - a swipe down from the top edge  -> kTop
//   - a swipe left from the right edge -> kRight
//   (Windows claims right-edge swipes for its own panel unless the AllowEdgeSwipe policy is 0.)
//
// Each gesture posts `msg` to `target` with wParam = kTop or kRight.
#pragma once
#include <windows.h>

namespace halo::edge {

constexpr WPARAM kTop = 1, kRight = 2;

bool Start(HINSTANCE inst, HWND target, UINT msg);  // UI thread
void Stop();
void OnDisplayChange();  // UI thread: re-measure the monitor and re-place the strips

}  // namespace halo::edge
