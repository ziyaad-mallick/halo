#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Both functions expect COM to be initialized on the calling thread
// (CoInitializeEx with COINIT_APARTMENTTHREADED or COINIT_MULTITHREADED).

struct PinnedApp {
    std::wstring name;        // display name (the .lnk file name without extension)
    std::wstring linkPath;    // full path to the .lnk
    std::wstring targetPath;  // resolved target (may be empty for shell items like Store apps)
    std::wstring aumid;       // AppUserModelID when known (may be empty)
};

struct IconImage {
    int width = 0, height = 0;          // 256x256 when available
    std::vector<uint8_t> bgra;          // premultiplied BGRA, width*height*4 bytes
};

// Pinned taskbar shortcuts, in the folder order Windows stores them:
// %APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar\*.lnk
// The order is directory enumeration order, not the taskbar's visual order.
std::vector<PinnedApp> LoadPinnedApps();

// Largest available icon for a file or .lnk (256x256 "jumbo" via the shell
// system image list SHIL_JUMBO, falling back to SHIL_EXTRALARGE 48x48).
// Returns false on failure.
bool LoadJumboIcon(const std::wstring& path, IconImage& out);
