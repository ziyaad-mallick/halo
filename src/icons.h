#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Both functions expect COM to be initialized on the calling thread
// (CoInitializeEx with COINIT_APARTMENTTHREADED or COINIT_MULTITHREADED).

struct PinnedApp {
    std::wstring name;        // display name (the .lnk file name without extension)
    std::wstring linkPath;    // full path to the .lnk, or L"shell:AppsFolder\<AUMID>" for Store apps
                              // (both work with LoadJumboIcon and ShellExecute)
    std::wstring aumid;       // AppUserModelID; empty if the shortcut has none
    std::wstring targetPath;  // resolved target (may be empty for shell items like Store apps)
};

struct IconImage {
    int width = 0, height = 0;          // 256x256 when available
    std::vector<uint8_t> bgra;          // premultiplied BGRA, width*height*4 bytes
};

// Pinned taskbar apps in the taskbar's visual order, Store apps included, read
// from HKCU\...\Explorer\Taskband\Favorites. If that is missing or unparseable,
// falls back to %APPDATA%\Microsoft\Internet Explorer\Quick Launch\User Pinned\
// TaskBar\*.lnk in directory order (desktop shortcuts only).
std::vector<PinnedApp> LoadPinnedApps();

// Largest available icon for a file, .lnk or shell:AppsFolder\<AUMID> item (256x256 "jumbo" via the shell
// system image list SHIL_JUMBO, falling back to SHIL_EXTRALARGE 48x48).
// Packaged apps (shell:AppsFolder) get the unplated Square44x44Logo the taskbar shows, read from
// the package; if that fails, or taskbarLogo is false, the Start tile from the shell.
// Returns false on failure.
bool LoadJumboIcon(const std::wstring& path, IconImage& out, bool taskbarLogo = true);
