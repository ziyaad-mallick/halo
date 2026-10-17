// Windows key pressed and released ALONE -> open PowerToys Command Palette
// and reveal the dock, instead of the Start menu. Win+<key> combos are untouched.
#pragma once
#include <windows.h>

namespace halo::winkey {

// Install the WH_KEYBOARD_LL hook on its own thread. On a lone Win tap the
// hook posts Message() to `target`.
bool Install(HWND target);
void Remove();

UINT Message();               // registered window message posted to `target`
void OpenPalette();           // sends Win+Alt+Space; call when Message() arrives

// The decision logic, separate from the hook so it can be unit tested.
struct Tracker {
    bool winDown = false, other = false;
    // Feed one key event; returns true when this was the Win release of a lone tap.
    bool Feed(DWORD vk, bool down) {
        if (vk == VK_LWIN || vk == VK_RWIN) {
            if (down) {
                if (!winDown) { winDown = true; other = false; }
                return false;
            }
            bool lone = winDown && !other;
            winDown = false;
            return lone;
        }
        if (down && winDown) other = true;
        return false;
    }
};

}  // namespace halo::winkey
