#include "taskbar.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <vector>

namespace halo::taskbar {
namespace {

constexpr UINT kUnknown = 0xFFFFFFFF;

std::wstring StatePath() { return DataDir() + L"\\taskbar-state.ini"; }

// Collect Shell_TrayWnd (primary) and every Shell_SecondaryTrayWnd.
std::vector<HWND> TaskbarWindows() {
    std::vector<HWND> v;
    if (HWND p = FindWindowW(L"Shell_TrayWnd", nullptr)) v.push_back(p);
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr)
        v.push_back(h);
    return v;
}

UINT ReadSavedState(bool* hiddenByUs) {
    const std::wstring p = StatePath();
    UINT s = GetPrivateProfileIntW(L"taskbar", L"state", kUnknown, p.c_str());
    if (hiddenByUs) *hiddenByUs = GetPrivateProfileIntW(L"taskbar", L"hidden", 0, p.c_str()) != 0;
    return s;
}

void WriteSaved(UINT state, bool hidden) {
    const std::wstring p = StatePath();
    if (state != kUnknown)
        WritePrivateProfileStringW(L"taskbar", L"state", std::to_wstring(state).c_str(), p.c_str());
    WritePrivateProfileStringW(L"taskbar", L"hidden", hidden ? L"1" : L"0", p.c_str());
}

UINT GetState() {
    APPBARDATA abd{sizeof(abd)};
    return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
}

void SetState(HWND hwnd, UINT state) {
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = hwnd;
    abd.lParam = static_cast<LPARAM>(state);
    SHAppBarMessage(ABM_SETSTATE, &abd);
}

}  // namespace

std::wstring DataDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? buf : L".";
    dir += L"\\Halo";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void HideAll() {
    bool hiddenByUs = false;
    UINT saved = ReadSavedState(&hiddenByUs);
    // Only capture the original when we are not already hiding it: after a
    // guardian crash the live state is our own auto-hide, not the user's.
    if (!hiddenByUs || saved == kUnknown) {
        UINT cur = GetState();
        WriteSaved(cur, true);
    } else {
        WriteSaved(kUnknown, true);
    }
    auto wins = TaskbarWindows();
    if (wins.empty()) return;
    SetState(wins.front(), ABS_AUTOHIDE);
    for (HWND w : wins) ShowWindow(w, SW_HIDE);
}

void RestoreAll() {
    bool hiddenByUs = false;
    UINT saved = ReadSavedState(&hiddenByUs);
    auto wins = TaskbarWindows();
    if (!wins.empty() && saved != kUnknown && hiddenByUs)
        SetState(wins.front(), saved);
    for (HWND w : wins) ShowWindow(w, SW_SHOWNA);
    if (saved != kUnknown || hiddenByUs) WriteSaved(kUnknown, false);
}

}  // namespace halo::taskbar
