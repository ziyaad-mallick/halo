#pragma once
// Minimize / restore swoop for windows the Halo Genie Windhawk mod can't reach (Chromium apps never load
// windhawk.dll; a machine without Windhawk has no mod at all).
//
// For each tracked window whose process hasn't loaded the mod, DWM's own transitions are switched off (so the
// stock minimize doesn't double up), and Halo draws the move itself: a click-through topmost overlay holding a
// DWM thumbnail of the window, shrinking and fading into its dock icon (and growing back out of it on a dock
// click). The overlay lives only while an animation runs, driven by a timer that exists only for that time:
// idle costs nothing. A window that is hung or elevated, a hidden dock, another monitor, or reduce_motion all
// mean no swoop (the window just minimizes, with no stock animation either).
#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <utility>

namespace halo {

class Swoop {
public:
    // Where `win`'s dock icon is, in physical screen px, for a swoop of a window at `from`; false = don't swoop.
    using IconFor = std::function<bool(HWND win, RECT const& from, RECT& icon)>;
    Swoop(IconFor iconFor, bool enabled);
    ~Swoop();  // gives every window its own transitions back

    void SetEnabled(bool on);  // reduce_motion flips this
    void Track(HWND hwnd);     // a window the tracker now shows
    void Forget(HWND hwnd);    // ... that it dropped
    void ModAsked(HWND hwnd);  // the genie mod is animating this one itself

    // Dock click on a minimized window: grows it out of its icon and calls `restore` once it is nearly full
    // size. False (restore not called) if no swoop applies; the caller restores at once.
    bool Restore(HWND hwnd, std::function<void()> restore);

    void OnMinimize(HWND hwnd);  // EVENT_SYSTEM_MINIMIZESTART
    void OnFrame();              // the overlay's timer

private:
    struct Anim;
    void Disable(HWND hwnd);
    void Enable(HWND hwnd);
    bool Start(HWND hwnd, RECT const& from, RECT const& icon, bool leaving, std::function<void()> action);
    void End();

    IconFor m_iconFor;
    bool m_on;
    std::map<HWND, bool> m_wins;           // every tracked window; true = we switched its transitions off
    std::map<HWND, ULONGLONG> m_modAsked;  // when the mod last asked about a window
    std::map<DWORD, std::pair<ULONGLONG, bool>> m_hasMod;  // pid -> (when checked, mod loaded or unknowable)
    std::unique_ptr<Anim> m_anim;
    void* m_hook = nullptr;
};

}  // namespace halo
