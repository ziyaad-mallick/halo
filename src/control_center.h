// Control Center: a frosted-glass panel at the top-right of the screen, opened by Win+A or by
// resting the pointer in a thin hot zone on the right edge. It lives on Halo's compositor but
// owns its own window, which exists only while the panel is open.
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
    RECT PanelRectScreen() const;  // screen px; empty while the panel is not shown
    // Called with true when the panel is shown or changes size, false when it starts closing.
    void SetVisibilityCallback(std::function<void(bool)> fn);

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace halo
