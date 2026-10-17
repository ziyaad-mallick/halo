#include "winkey.h"

namespace halo::winkey {
namespace {

constexpr ULONG_PTR kMine = 0x48414C4F;  // "HALO": marks input we injected
constexpr WORD kUnassigned = 0xE8;       // VK with no meaning; cancels the Start menu tap

HHOOK g_hook = nullptr;
HWND g_target = nullptr;
bool g_testMode = false;  // HALO_TEST_WINKEY=1: also handle injected input (tests drive it with SendInput)
Tracker g_track;

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
        if (k->dwExtraInfo != kMine && (!(k->flags & LLKHF_INJECTED) || g_testMode)) {
            bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
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

bool Install(HWND target) {
    g_target = target;
    wchar_t v[8] = {};
    g_testMode = GetEnvironmentVariableW(L"HALO_TEST_WINKEY", v, 8) && v[0] == L'1';
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevel, GetModuleHandleW(nullptr), 0);
    return g_hook != nullptr;
}

void Remove() {
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
}

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
