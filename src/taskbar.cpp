#include "taskbar.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cwchar>
#include <map>
#include <vector>

namespace halo::taskbar {
namespace {

constexpr UINT kUnknown = 0xFFFFFFFF;

TestTarget* g_test = nullptr;

// Regions a taskbar window had before we emptied it. A real taskbar has none, so this stays empty
// outside tests; it is in memory only (a lost entry restores to "no region").
std::map<HWND, HRGN> g_savedRgn;

std::wstring StatePath() { return g_test ? g_test->stateFile : DataDir() + L"\\taskbar-state.ini"; }

// Collect Shell_TrayWnd (primary) and every Shell_SecondaryTrayWnd.
std::vector<HWND> TaskbarWindows() {
    if (g_test) return g_test->find();
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
    if (g_test) return g_test->appBarState;
    APPBARDATA abd{sizeof(abd)};
    return static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
}

void SetState(HWND hwnd, UINT state) {
    if (g_test) {
        g_test->appBarState = state;
        return;
    }
    APPBARDATA abd{sizeof(abd)};
    abd.hWnd = hwnd;
    abd.lParam = static_cast<LPARAM>(state);
    SHAppBarMessage(ABM_SETSTATE, &abd);
}

// guardian.exe is linked without gdi32 (tools/build.sh is not ours to change), so the two GDI calls we
// need, to make and free a region, are resolved at run time. gdi32 is always loaded in a process that has user32.
struct GdiFns {
    HRGN(WINAPI* create)(int, int, int, int) = nullptr;
    BOOL(WINAPI* del)(HGDIOBJ) = nullptr;
};
const GdiFns& Gdi() {
    static const GdiFns f = [] {
        GdiFns g;
        if (HMODULE m = LoadLibraryW(L"gdi32.dll")) {
            g.create = reinterpret_cast<decltype(g.create)>(reinterpret_cast<void*>(GetProcAddress(m, "CreateRectRgn")));
            g.del = reinterpret_cast<decltype(g.del)>(reinterpret_cast<void*>(GetProcAddress(m, "DeleteObject")));
        }
        return g;
    }();
    return f;
}
HRGN NewRgn() { return Gdi().create ? Gdi().create(0, 0, 0, 0) : nullptr; }
void FreeRgn(HRGN r) {
    if (r && Gdi().del) Gdi().del(r);
}

// GetWindowRgnBox: ERROR (0) = no region, NULLREGION (1) = an empty one, SIMPLEREGION/COMPLEXREGION = real.
// It also fails outright now and then while another thread shows or hides the window (ERROR with a last-error
// code, ERROR_INVALID_HANDLE; seen in tools/taskbar_test.cpp, also on a window no Halo code touches). That is
// "unknown", not "no region": acting on it would replace a good empty region, and SetWindowRgn on a visible
// window passes through a moment with none. So read up to 3 times and keep "unreadable" apart.
enum class Rgn { None, Empty, Real, Unreadable };
Rgn RegionState(HWND h) {
    for (int i = 0; i < 3; ++i) {
        RECT box{};
        SetLastError(0);
        const int t = GetWindowRgnBox(h, &box);
        if (t == ERROR && GetLastError() != 0) continue;
        return t == ERROR ? Rgn::None : t == NULLREGION ? Rgn::Empty : Rgn::Real;
    }
    return Rgn::Unreadable;
}
bool HasEmptyRegion(HWND h) { return RegionState(h) == Rgn::Empty; }

// Drop remembered regions of windows that no longer exist (Explorer restarted).
void ForgetDeadWindows() {
    for (auto it = g_savedRgn.begin(); it != g_savedRgn.end();) {
        if (!IsWindow(it->first)) {
            FreeRgn(it->second);
            it = g_savedRgn.erase(it);
        } else {
            ++it;
        }
    }
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

void SetTestTarget(TestTarget* t) { g_test = t; }

bool IsTaskbarClass(const wchar_t* cls) {
    return cls && (wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0);
}

bool IsConcealed(HWND h) { return IsWindow(h) && !IsWindowVisible(h) && HasEmptyRegion(h); }

bool ConcealWindow(HWND h) {
    if (!IsWindow(h)) return false;
    bool ok = true;
    const bool visible = IsWindowVisible(h) != FALSE;
    // Hide first: replacing a region on a window that is on screen passes through a moment with none.
    if (visible) ShowWindow(h, SW_HIDE);
    const Rgn st = RegionState(h);
    if (st == Rgn::None || st == Rgn::Real) {  // Empty: already ours; Unreadable: leave a region we cannot see alone
        HRGN orig = NewRgn();
        const int t = orig ? GetWindowRgn(h, orig) : ERROR;
        if ((t == SIMPLEREGION || t == COMPLEXREGION) && !g_savedRgn.count(h)) g_savedRgn[h] = orig;
        else FreeRgn(orig);
        HRGN empty = NewRgn();
        // On success the window owns `empty`; redraw only matters while something is on screen.
        if (!empty || !SetWindowRgn(h, empty, FALSE)) {
            FreeRgn(empty);
            ok = false;
        }
    }
    return ok;
}

void ReleaseWindow(HWND h) {
    auto it = g_savedRgn.find(h);
    if (!IsWindow(h)) {
        if (it != g_savedRgn.end()) {
            FreeRgn(it->second);
            g_savedRgn.erase(it);
        }
        return;
    }
    // Only undo our own empty region: if Explorer has put a region of its own there since, that one is right.
    // An unreadable region is cleared too: a taskbar stuck with no pixels is the worse mistake.
    const Rgn st = RegionState(h);
    if (st == Rgn::Empty || st == Rgn::Unreadable) {
        const bool visible = IsWindowVisible(h) != FALSE;
        HRGN back = it != g_savedRgn.end() ? it->second : nullptr;
        if (it != g_savedRgn.end()) g_savedRgn.erase(it);
        if (!SetWindowRgn(h, back, visible)) FreeRgn(back);
    } else if (it != g_savedRgn.end()) {
        FreeRgn(it->second);
        g_savedRgn.erase(it);
    }
    ShowWindow(h, SW_SHOWNA);
}

bool ReconcealIfShown(HWND h) {
    if (!IsWindow(h)) return false;
    if (!IsWindowVisible(h)) {
        const Rgn st = RegionState(h);
        if (st == Rgn::Empty || st == Rgn::Unreadable) return false;
    }
    ConcealWindow(h);
    return true;
}

bool HideAll() {
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
    if (wins.empty()) return true;
    ForgetDeadWindows();
    SetState(wins.front(), ABS_AUTOHIDE);
    bool all = true;
    for (HWND w : wins) all = ConcealWindow(w) && all;
    return all;
}

void RestoreAll() {
    bool hiddenByUs = false;
    UINT saved = ReadSavedState(&hiddenByUs);
    auto wins = TaskbarWindows();
    if (!wins.empty() && saved != kUnknown && hiddenByUs)
        SetState(wins.front(), saved);
    for (HWND w : wins) ReleaseWindow(w);
    ForgetDeadWindows();
    if (saved != kUnknown || hiddenByUs) WriteSaved(kUnknown, false);
}

}  // namespace halo::taskbar
