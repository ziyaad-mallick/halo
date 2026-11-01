#pragma once
// Halo's out-of-process minimize / dock-restore animation. A click-through
// overlay bends a live DWM thumbnail using compositor animation when supported;
// a timer-driven thumbnail renderer handles unsupported Windows builds.
// Windows that cannot supply DWM thumbnails may not animate. Restore from
// outside Halo's dock is not intercepted. Reduced motion disables the effect.
#include <windows.h>

#include <functional>
#include <map>
#include <memory>
#include <utility>

namespace halo {

class SwoopDevice;

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
#ifdef HALO_SWOOP_TEST
    bool UsingCompositorForTest() const;
#endif

private:
    struct Anim;
    static DWORD WINAPI FrameThread(void* context);
    static bool RenderFrame(Anim& animation, float t);
    void Disable(HWND hwnd);
    void Enable(HWND hwnd);
    bool Start(HWND hwnd, RECT const& from, RECT const& icon, bool leaving, std::function<void()> action, POINT home = {LONG_MIN, 0});
    static void MoveHome(Anim& animation);
    void End();

    IconFor m_iconFor;
    std::shared_ptr<SwoopDevice> m_device;
    bool m_on;
    std::map<HWND, bool> m_wins;           // every tracked window; true = we switched its transitions off
    std::map<HWND, ULONGLONG> m_modAsked;  // when the mod last asked about a window
    std::shared_ptr<Anim> m_anim;
    void* m_hook = nullptr;
};

}  // namespace halo
