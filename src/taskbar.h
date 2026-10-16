// Native Windows taskbar hide/restore. Used only by guardian.exe.
#pragma once
#include <string>

namespace halo::taskbar {

// Remember the user's current appbar state (first time only), switch every
// taskbar to auto-hide and hide the windows. Safe to call repeatedly.
void HideAll();

// Put back the state recorded before we hid it and show every taskbar window.
void RestoreAll();

// "%LOCALAPPDATA%\Halo" (created on demand).
std::wstring DataDir();

}  // namespace halo::taskbar
