// Control Center: a frosted-glass panel at the top-right of the screen, opened by Win+A or by the
// top edge gestures in edge.cpp (pointer resting in the top-right corner, touch swipe down from the
// top). It lives on Halo's compositor but owns its own window, which exists only while it is open.
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

    // Where the panel is on screen (physical px); empty while it is closed. For things that stack
    // below it (notifications). Height changes when sections expand: see onResized.
    RECT PanelRectScreen() const;
    HWND Hwnd() const;  // null while closed
    // UI thread. onShown: the panel just opened. onHidden: it started closing (fading out).
    // onResized: it opened or changed height while open; read PanelRectScreen() again.
    std::function<void()> onShown, onHidden, onResized;
    // Focus moving to this window must not close the panel (the attached notification column).
    std::function<HWND()> siblingHwnd;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace halo
