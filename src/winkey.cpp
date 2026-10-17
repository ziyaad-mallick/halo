#include "winkey.h"

#include <atomic>

namespace halo::winkey {
namespace {

constexpr ULONG_PTR kMine = 0x48414C4F;  // "HALO": marks input we injected
constexpr WORD kUnassigned = 0xE8;       // VK with no meaning; cancels the Start menu tap

HHOOK g_hook = nullptr;
HANDLE g_thread = nullptr;
DWORD g_threadId = 0;
HWND g_target = nullptr;
std::atomic<bool> g_watchEsc{false};  // the launcher panel is up: report Esc
bool g_testMode = false;  // HALO_TEST_WINKEY=1: also handle injected input (tests drive it with SendInput)
Tracker g_track;
bool g_swallowA = false;  // Win+A went down and we swallowed it; swallow its repeats and release too

void Key(INPUT& in, WORD vk, bool up) {
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    in.ki.dwExtraInfo = kMine;
}

// Runs inside every keystroke: keep it tiny (LowLevelHooksTimeout, ~300 ms, silently drops slow hooks).
LRESULT CALLBACK LowLevel(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION) {
        auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
        if (g_watchEsc && k->vkCode == VK_ESCAPE && (w == WM_KEYDOWN || w == WM_SYSKEYDOWN) && k->dwExtraInfo != kMine)
            PostMessageW(g_target, EscapeMessage(), 0, 0);
        if (k->dwExtraInfo != kMine && (!(k->flags & LLKHF_INJECTED) || g_testMode)) {
            bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
            // Win+A belongs to Halo's Control Center while the hook runs (RegisterHotKey can't take it
            // from the shell). Feed() first so the Win release no longer counts as a lone tap, then cancel
            // the Start menu with an unassigned key exactly like a tap does.
            if (k->vkCode == 'A' && (g_swallowA || (down && g_track.winDown))) {
                if (down && !g_swallowA) {
                    g_swallowA = true;
                    g_track.Feed(k->vkCode, true);
                    INPUT in[2] = {};
                    Key(in[0], kUnassigned, false);
                    Key(in[1], kUnassigned, true);
                    SendInput(2, in, sizeof(INPUT));
                    PostMessageW(g_target, ControlCenterMessage(), 0, 0);
                } else if (!down) {
                    g_swallowA = false;
                }
                return 1;
            }
            if (g_track.Feed(k->vkCode, down)) {
                // Swallow this Win release and re-inject it right after an unassigned key, in one
                // SendInput so the order is certain: the Start menu only opens on a Win release
                // with nothing in between.
                INPUT in[3] = {};
                Key(in[0], kUnassigned, false);
                Key(in[1], kUnassigned, true);
                Key(in[2], static_cast<WORD>(k->vkCode), true);
                SendInput(3, in, sizeof(INPUT));
                PostMessageW(g_target, Message(), 0, 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, w, l);
}

}  // namespace

UINT Message() {
    static UINT m = RegisterWindowMessageW(L"Halo.WinKeyTap");
    return m;
}

UINT ControlCenterMessage() {
    static UINT m = RegisterWindowMessageW(L"Halo.WinA");
    return m;
}

// The hook gets its own thread: Windows makes every keystroke wait for a
// low-level hook, so it must never sit behind a busy UI thread.
DWORD WINAPI HookThread(LPVOID ready) {
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevel, GetModuleHandleW(nullptr), 0);
    SetEvent(static_cast<HANDLE>(ready));
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
    return 0;
}

bool Install(HWND target) {
    g_target = target;
    wchar_t v[8] = {};
    g_testMode = GetEnvironmentVariableW(L"HALO_TEST_WINKEY", v, 8) && v[0] == L'1';
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = CreateThread(nullptr, 0, HookThread, ready, 0, &g_threadId);
    if (g_thread) WaitForSingleObject(ready, 2000);
    CloseHandle(ready);
    return g_hook != nullptr;
}

void Remove() {
    if (!g_thread) return;
    PostThreadMessageW(g_threadId, WM_QUIT, 0, 0);
    WaitForSingleObject(g_thread, 2000);
    CloseHandle(g_thread);
    g_thread = nullptr;
}

UINT EscapeMessage() {
    static UINT m = RegisterWindowMessageW(L"Halo.WinKeyEscape");
    return m;
}

void WatchEscape(bool on) { g_watchEsc = on; }

void OpenPalette() {
    // Win is already released (the tap's Win-up went through before this message ran).
    INPUT in[6] = {};
    Key(in[0], VK_LWIN, false);
    Key(in[1], VK_MENU, false);
    Key(in[2], VK_SPACE, false);
    Key(in[3], VK_SPACE, true);
    Key(in[4], VK_MENU, true);
    Key(in[5], VK_LWIN, true);
    SendInput(6, in, sizeof(INPUT));
}

}  // namespace halo::winkey
