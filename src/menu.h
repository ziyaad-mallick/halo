#pragma once
// The dock's right-click menu: a small light-glass popup above the clicked icon (the same recipe as the
// Launcher's Power menu: the dock's glass, ink and hover pills from theme.h). A top-level window on the
// dock's compositor that never takes activation (WS_EX_NOACTIVATE), so the app behind keeps focus.
//
// It closes on Esc (the host forwards the keyboard hook's Esc message), on a click anywhere outside it
// (the menu holds the mouse capture while open), when another app takes the foreground (a WinEvent
// hook, not a timer) and when an item is chosen. Everything is event driven.

#include <windows.h>
#include <dwmapi.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include <functional>
#include <string>
#include <vector>

namespace halo {

namespace wuc = winrt::Windows::UI::Composition;

struct MenuEntry {
    std::wstring label;
    bool separator = false;
    std::function<void()> action;  // runs after the menu has closed
};

struct MenuHost {
    // The dock's glass: the argument is the grain's height in px, then the corner radius in px.
    std::function<wuc::ContainerVisual(int, float)> glass;
    std::function<void()> opened, closed;  // the host holds the dock up while the menu is open
};

class Menu {
public:
    Menu(HINSTANCE inst, wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev, MenuHost host);
    ~Menu();

    // Opens the menu centered on screen x `centerX`, `gap` px above `pillTop`, clamped to `monitor`.
    // Replaces a menu that is already up.
    void Show(std::vector<MenuEntry> entries, int centerX, int pillTop, RECT const& monitor, float scale);
    void Close(bool instant = false);
    bool IsOpen() const { return m_open; }
    void Raise();  // stay above the dock after it re-asserts topmost

    // Test hooks (HALO_TEST): hover row `row` / choose it, as the pointer would.
    void TestHover(int row);
    void TestActivate(int row);

private:
    struct Row {
        RECT r;        // client px: the hover pill
        bool separator;
        std::wstring label;
    };
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT, WPARAM, LPARAM);
    int RowAt(int x, int y) const;
    void SetHover(int row);
    void Choose(int row);
    void Finish();

    wuc::Compositor m_comp;
    wuc::CompositionGraphicsDevice m_gdev;
    MenuHost m_host;
    HWND m_hwnd{};
    wuc::Desktop::DesktopWindowTarget m_target{nullptr};
    wuc::ContainerVisual m_root{nullptr};
    wuc::ContainerVisual m_menu{nullptr};
    wuc::ShapeVisual m_hoverPlate{nullptr};
    wuc::CubicBezierEasingFunction m_easeOut{nullptr};
    winrt::com_ptr<IDWriteFactory> m_dwrite;
    HWINEVENTHOOK m_hook{};

    std::vector<Row> m_rows;
    std::vector<MenuEntry> m_entries;
    float m_s = 1.0f;
    int m_w = 0, m_h = 0;
    int m_hover = -1;
    bool m_open = false;
    unsigned m_gen = 0;  // invalidates a fade out that a new Show interrupted
    friend void CALLBACK MenuForegroundProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
};

}  // namespace halo
