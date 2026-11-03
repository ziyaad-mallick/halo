#pragma once
// The notifications + calendar column of Start (HALO 2.0): a header with Clear, the notification cards (newest
// first, scrolling), and a month calendar at the bottom. It is a visual, not a window: the Launcher mounts it at
// the left of its panel, forwards the pointer to it (in column-local physical px) and closes with it.
//
// Memory: the surfaces exist only between Build and Release; the notification feed's thread sleeps unless the
// column is active (Start open).

#include <windows.h>
#include <winrt/Windows.UI.Composition.h>

#include <functional>
#include <memory>

namespace halo {

class NotifColumn {
public:
    NotifColumn(HINSTANCE inst, winrt::Windows::UI::Composition::Compositor comp,
                winrt::Windows::UI::Composition::CompositionGraphicsDevice gdev);
    ~NotifColumn();

    // A w x h px (scale `scale`) column; the caller positions the returned visual. Rebuilds from scratch.
    winrt::Windows::UI::Composition::ContainerVisual Build(float w, float h, float scale);
    void Release();
    void SetActive(bool on);  // Start shown: read the notifications now and keep them current; false: stop

    // Pointer, column-local physical px. Click returns true when it hit something of the column's.
    void Move(float x, float y);
    void Leave();
    bool Click(float x, float y);
    bool Wheel(float x, float y, int delta);
    bool Contains(float x, float y) const;

    // A card opened its app: the host may close Start.
    std::function<void()> onOpened;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

}  // namespace halo
