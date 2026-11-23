// Native Windows taskbar hide/restore. Used by guardian.exe (hide, re-hide, restore) and halo.exe
// (restore only, when it has no guardian left).
//
// Hiding is layered, so that a taskbar window Explorer shows again has nothing to draw even before
// the guardian reacts:
//   1. ABM_SETSTATE ABS_AUTOHIDE      - frees the work area; Explorer parks the window off the edge.
//   2. SetWindowRgn(empty region)     - the window has no visible pixels and no hit area, whatever
//                                       Explorer does to its visibility. The region survives ShowWindow.
//   3. ShowWindow(SW_HIDE)            - as before.
// Everything is undone by RestoreAll(), which finds the windows we emptied by the empty region itself
// (a real taskbar never has one), so it also works after a crash that lost our in-memory state.
#pragma once
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace halo::taskbar {

// Remember the user's current appbar state (first time only), switch every taskbar to auto-hide,
// empty and hide the windows. Safe to call repeatedly. False if a window refused the empty region
// (it is then only hidden, as before).
bool HideAll();

// Put back the state recorded before we hid it, clear our empty region and show every taskbar window.
void RestoreAll();

// "%LOCALAPPDATA%\Halo" (created on demand).
std::wstring DataDir();

// ---- one window at a time (the guardian's event hook uses these; tools/taskbar_test.cpp runs them on a fake window)

bool IsTaskbarClass(const wchar_t* cls);  // Shell_TrayWnd / Shell_SecondaryTrayWnd

// Empty region + hidden. Idempotent and cheap when already concealed. Remembers a region the window
// had before (in memory). False if the region could not be set.
bool ConcealWindow(HWND h);

// Undo ConcealWindow: put the remembered region back (or none), then show without activating.
// Leaves a region that is not our empty one alone.
void ReleaseWindow(HWND h);

// True if the window is hidden with an empty region.
bool IsConcealed(HWND h);

// For a "window shown / moved" event: conceal the window if Explorer made it visible or removed our
// region. Returns true if it had to act.
bool ReconcealIfShown(HWND h);

// ---- test seam: aim HideAll/RestoreAll at fake windows, a private state file and a fake appbar state,
// so no test can touch the real taskbar. Pass nullptr to go back to the real thing.
struct TestTarget {
    std::function<std::vector<HWND>()> find;
    std::wstring stateFile;
    UINT appBarState = 0;  // stands in for ABM_GETSTATE / ABM_SETSTATE
};
void SetTestTarget(TestTarget* t);

}  // namespace halo::taskbar
