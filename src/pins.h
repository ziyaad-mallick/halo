#pragma once
// Halo's own pin list: %LOCALAPPDATA%\Halo\pins.txt (override: HALO_PINS_FILE), one launch path per
// line, UTF-8, in dock order. A launch path is what PinnedApp::linkPath holds: a .lnk, an exe, or
// shell:AppsFolder\<AUMID>. The first time there is no file, it is seeded once from the Windows
// taskbar pins (LoadPinnedApps); after that the taskbar is never read again.
// Needs COM on the calling thread, like icons.h.

#include "icons.h"

#include <string>
#include <vector>

namespace halo::pins {

std::wstring FilePath();                  // where the list lives
std::vector<PinnedApp> Load();            // the list, seeding the file first if it does not exist
bool IsPinned(std::wstring const& path);  // case-insensitive match on the launch path
bool Pin(std::wstring const& path);       // appends and rewrites the file; false if already pinned or on error
bool Unpin(std::wstring const& path);     // removes and rewrites the file; false if not pinned or on error

}  // namespace halo::pins
