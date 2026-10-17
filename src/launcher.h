#pragma once
// The Launcher panel: a glass panel that rises above the dock while PowerToys
// Command Palette (opened by the Windows key or the Start logo) has the
// keyboard. It never takes activation, so the palette keeps typing.
//
//   Recent apps  - up to 8, an MRU kept in %LOCALAPPDATA%\Halo\recent.txt
//   Recent files - up to 6 from the shell's Recent folder, newest first
//   user · Settings · Power (Sleep, Restart, Shut down, Lock, Sign out)
//
// A top-level window of its own on the dock's compositor (the dock's window is
// only one pill tall). Event-driven: a foreground WinEvent hook exists only
// while the panel is up, and nothing polls.
//
// HALO_TEST=1 or HALO_TEST_POWER_DRYRUN=1: power actions are logged, never run.

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include <chrono>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "icons.h"

namespace halo {

namespace wuc = winrt::Windows::UI::Composition;

// What the panel needs from the dock that owns it.
struct LauncherHost {
    // The dock's glass (blur, tint, grain, rim), sized by its parent. The argument is the grain's height in px.
    std::function<wuc::ContainerVisual(int)> glass;
    // The dock's icon brush for a launch target (a .lnk, shell:AppsFolder\<AUMID> or exe), null if it has none.
    std::function<wuc::CompositionBrush(std::wstring const&)> appIcon;
    // Brings the app to the front if it is running, else starts it.
    std::function<void(std::wstring const&)> launch;
    // The panel has gone (the dock may auto-hide again).
    std::function<void()> closed;
};

// Where the panel goes: above the dock, in screen px.
struct LauncherAnchor {
    int centerX = 0;
    int bottom = 0;         // the panel's bottom edge
    float dockWidth = 0.0f; // the dock's width at rest
    float scale = 1.0f;
    RECT monitor{};
};

class Launcher {
public:
    Launcher(HINSTANCE inst, wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev,
             LauncherHost host);
    ~Launcher();

    // `standalone`: no Command Palette behind it (tests); only a click elsewhere, Esc or a toggle closes it.
    void Show(LauncherAnchor const& anchor, bool standalone = false);
    void Hide();
    bool IsOpen() const { return m_open; }

    // Remembers an app for "Recent apps" (a launch target as the host's `launch` takes it).
    void Note(std::wstring const& target);

    // HALO_TEST: wParam 2 = toggle the Power menu, 3+n = run Power item n (dry run), 9 = pretend the desktop
    // took the foreground, 10 = pretend Command Palette did.
    void TestCommand(WPARAM w);

    void OnForeground(HWND fg);  // WinEvent callback target
    void Raise();                // back to the top of the topmost band

private:
    enum class Kind { App, File, Settings, Power, MenuItem };
    struct Hit {
        D2D1_RECT_F r;
        Kind kind;
        int index;
    };
    struct FileEntry {
        std::wstring name, path;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT, WPARAM, LPARAM);
    void CreateHwnd(HINSTANCE inst);
    void Build();  // lays out and draws everything for the current content
    void DrawPanel(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawMenu(ID2D1DeviceContext* dc, float ox, float oy);
    wuc::CompositionDrawingSurface MakeSurface(float w, float h);
    wuc::SpriteVisual MakeSurfaceVisual(wuc::CompositionDrawingSurface const& s, float w, float h);
    void LoadFiles();
    void Prefetch();
    void Refresh();
    void Relayout();
    void Place();
    void Stack(HWND palette);
    void Glue(RECT const& r);
    HWND PaletteWindow() const;
    static std::wstring IconKey(std::wstring const& path);
    void LoadMru();
    void SaveMru() const;
    void Animate(wuc::CompositionObject const& target, wchar_t const* prop, float to, int ms);
    void Highlight(D2D1_RECT_F const* r, float radius, bool inMenu);
    Hit const* HitAt(float x, float y) const;
    void OnHover(float x, float y);
    void Click(Hit const& hit);
    void ToggleMenu(bool open);
    void RunPower(int item);
    void Close();  // after the fade
    bool IsPalette(HWND hwnd) const;

    wuc::Compositor m_comp;
    wuc::CompositionGraphicsDevice m_gdev;
    LauncherHost m_host;
    HWND m_hwnd{};
    wuc::Desktop::DesktopWindowTarget m_target{nullptr};
    wuc::ContainerVisual m_root{nullptr};
    wuc::ContainerVisual m_panel{nullptr};
    wuc::ContainerVisual m_content{nullptr};  // the panel's contents; dims behind the menu
    wuc::ShapeVisual m_hover{nullptr};
    wuc::CompositionRoundedRectangleGeometry m_hoverShape{nullptr};
    wuc::ContainerVisual m_menu{nullptr};
    wuc::ShapeVisual m_menuHover{nullptr};
    wuc::CompositionRoundedRectangleGeometry m_menuHoverShape{nullptr};
    wuc::CubicBezierEasingFunction m_easeOut{nullptr};

    winrt::com_ptr<IDWriteFactory> m_dwrite;
    std::map<std::wstring, IconImage> m_fileIcons;  // by extension, or path for per-file icons

    // Current layout (physical px).
    LauncherAnchor m_anchor;
    float m_s = 1.0f;
    float m_w = 0, m_h = 0;       // panel size
    float m_menuW = 0, m_menuH = 0, m_menuX = 0, m_menuY = 0;
    std::vector<Hit> m_hits, m_menuHits;
    D2D1_RECT_F m_appsLabel{}, m_filesLabel{}, m_divider{}, m_avatarRect{}, m_nameRect{}, m_settingsRect{}, m_powerRect{};
    std::vector<D2D1_RECT_F> m_appCells, m_fileRows, m_menuRows;  // icon rects, file rows, menu rows (menu coordinates)
    std::vector<std::wstring> m_mru;      // launch targets, newest first
    std::vector<std::wstring> m_apps;     // what the panel shows
    std::vector<FileEntry> m_files;
    std::wstring m_user;
    std::vector<uint8_t> m_avatar;        // BGRA premultiplied, m_avatarPx square; empty = draw an initial
    int m_avatarPx = 0;
    bool m_avatarTried = false;
    bool m_dryRun = false;
    float m_iconPx = 44;           // app icon size, logical px (shrinks under a tight palette)
    bool m_iconMiss = false;       // the last build lacked a file icon the cache didn't have yet
    float m_paletteBottom = 0;     // where the palette ends when the panel is glued under it
    float m_stackW = 0;            // the palette's width in px once seen: the panel matches it
    int m_glueTop = 0;             // screen y the panel hangs from when it could not be stacked
    HWND m_palette = nullptr;
    POINT m_stackAt{};
    D2D1_RECT_F m_vsep{};

    bool m_open = false;
    bool m_standalone = false;
    bool m_sawPalette = false;
    bool m_menuOpen = false;
    bool m_tracking = false;
    int m_hoverIndex = -2;        // index into the hits being hovered, -1 none
    bool m_hoverOn = false, m_menuHoverOn = false;  // the hover plates are showing
    int m_pressedIndex = -1;
    unsigned m_gen = 0;           // invalidates a fade-out that a reopen interrupted
    std::chrono::steady_clock::time_point m_shownAt{};
    HWINEVENTHOOK m_hook = nullptr;
};

}  // namespace halo
