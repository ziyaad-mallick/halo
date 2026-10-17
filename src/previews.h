#pragma once
// Hover previews: a glass strip above the dock with a live DWM thumbnail and the title of each
// window of the hovered app. A top-level window of its own on the dock's compositor that never
// takes activation (WS_EX_NOACTIVATE). Clicking a card activates that window; the x on a hovered
// card sends it WM_CLOSE. Thumbnails are registered only while the strip is up.

#include <windows.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace halo {

namespace wuc = winrt::Windows::UI::Composition;

struct PreviewsHost {
    std::function<wuc::ContainerVisual(int)> glass;  // the dock's glass; the argument is the grain's height in px
    std::function<void(HWND)> activate;              // a card was clicked
    std::function<void()> entered, left;             // the pointer entered / left the strip
    std::function<void()> refresh;                   // a window was closed from the strip: re-list
};

class Previews {
public:
    Previews(HINSTANCE inst, wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev, PreviewsHost host);
    ~Previews();

    // Shows (or, if already up, moves to) the strip for `windows`, centered on screen x `centerX`,
    // just above `pillTop`. Returns false if there was nothing to show.
    bool Show(std::vector<HWND> const& windows, int centerX, int pillTop, RECT const& monitor, float scale);
    void Hide(bool instant = false);
    bool IsVisible() const { return m_shown; }
    bool ContainsCursor() const;
    void Raise();

private:
    struct Card {
        HWND src;
        HTHUMBNAIL thumb;
        RECT card;  // client px
        RECT dst;   // thumbnail rect, client px, at rest
        std::wstring title;
    };
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT, WPARAM, LPARAM);
    void Clear();  // unregisters every thumbnail
    void ApplyThumbs(float p);  // p: 0 = faded and low, 1 = at rest
    void SetHover(int index);
    int CardAt(int x, int y) const;
    bool OnClose(int index, int x, int y) const;
    void Finish();

    wuc::Compositor m_comp;
    wuc::CompositionGraphicsDevice m_gdev;
    PreviewsHost m_host;
    HWND m_hwnd{};
    wuc::Desktop::DesktopWindowTarget m_target{nullptr};
    wuc::ContainerVisual m_root{nullptr};
    wuc::ContainerVisual m_strip{nullptr};
    wuc::ShapeVisual m_plate{nullptr};
    wuc::CompositionRoundedRectangleGeometry m_plateShape{nullptr};
    wuc::SpriteVisual m_closeBtn{nullptr};
    wuc::CubicBezierEasingFunction m_easeOut{nullptr};
    winrt::com_ptr<IDWriteFactory> m_dwrite;

    std::vector<Card> m_cards;
    float m_s = 1.0f;
    float m_w = 0, m_h = 0;
    int m_hover = -1;
    bool m_shown = false;  // up or fading in
    bool m_tracking = false;
    bool m_inside = false;
    float m_p = 0.0f;  // thumbnail progress
    bool m_fadeIn = true;
    std::chrono::steady_clock::time_point m_fadeStart{};
};

}  // namespace halo
