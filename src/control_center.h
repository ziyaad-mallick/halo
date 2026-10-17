// Control Center: a frosted-glass panel at the top-right of the screen, opened by Win+A, by resting
// the pointer in a thin hot zone on the right edge, by flicking the pointer into that edge, or by a
// touch swipe in from it. It lives on Halo's compositor but owns its own window, which exists only
// while the panel is open.
#pragma once
#include <windows.h>
#include <winrt/Windows.UI.Composition.h>
#include <functional>
#include <memory>

namespace halo {

class ControlCenter {
public:
    ControlCenter(HINSTANCE inst, HWND owner, winrt::Windows::UI::Composition::Compositor comp,
                  winrt::Windows::UI::Composition::CompositionGraphicsDevice gdev);
    ~ControlCenter();
    void Toggle();
    void Open();
    void Close();
    bool IsOpen() const;
    void OnDisplayChange();  // re-places the hot zone

    // Where the panel is on screen (physical px); empty while it is closed. For things that stack
    // below it (notifications). Height changes when sections expand: see onResized.
    RECT PanelRectScreen() const;
    // UI thread. onShown: the panel just opened. onHidden: it started closing (fading out).
    // onResized: it opened or changed height while open; read PanelRectScreen() again.
    std::function<void()> onShown, onHidden, onResized;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace halo
