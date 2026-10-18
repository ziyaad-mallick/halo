#pragma once
// The Launcher panel: Start. A glass panel that rises above the dock when the Windows key or the Start
// logo is pressed, and takes the keyboard (it is the only search UI):
//
//   Search bar   - type to filter Start-menu apps and recent files; Enter launches the top result
//   Recent apps  - up to 6, an MRU kept in %LOCALAPPDATA%\Halo\recent.txt
//   Recent files - 2 x 6 from the shell's Recent folder, newest first
//   user · Settings · Power (Sleep, Restart, Shut down, Lock, Sign out)
//
// A top-level window of its own on the dock's compositor (the dock's window is only one pill tall).
// Event-driven: it closes when it loses activation or on Esc; nothing polls (the caret blinks on the
// compositor).
//
// HALO_TEST=1 or HALO_TEST_POWER_DRYRUN=1: power actions are logged, never run. HALO_TEST_STAY=1: losing activation
// does not close the panel (screenshots while the desktop is in use).

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

    void Show(LauncherAnchor const& anchor);
    void Hide();
    bool IsOpen() const { return m_open; }

    // Remembers an app for "Recent apps" (a launch target as the host's `launch` takes it).
    void Note(std::wstring const& target);

    // HALO_TEST: wParam 2 = toggle the Power menu, 3+n = run Power item n (dry run), 9 = pretend the panel
    // lost activation (a click elsewhere), 20 = snap the hover plate and icon to where their animations end.
    void TestCommand(WPARAM w);

    void Raise();                // back to the top of the topmost band

private:
    enum class Kind { App, File, Result, Settings, Power, MenuItem };
    struct Hit {
        D2D1_RECT_F r;
        Kind kind;
        int index;
    };
    struct FileEntry {
        std::wstring name, path;
    };
    struct AppEntry {
        std::wstring name, lname, path;  // path: the Start-menu .lnk, which the host launches
    };
    struct Result {
        bool app;
        std::wstring name, path;
    };
    struct IconV {  // a hoverable icon: its own visual, so it scales and lifts on its own
        wuc::SpriteVisual v{nullptr};
        float restY = 0;
    };

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT, WPARAM, LPARAM);
    void CreateHwnd(HINSTANCE inst);
    void Build();      // lays out and draws everything for the current content
    void BuildBody();  // the part under the search bar: the home sections or the results list
    void DrawStatic(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawSearch(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawBody(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawMenu(ID2D1DeviceContext* dc, float ox, float oy);
    void Redraw(wuc::CompositionDrawingSurface const& s, void (Launcher::*draw)(ID2D1DeviceContext*, float, float));
    wuc::CompositionDrawingSurface MakeSurface(float w, float h);
    wuc::SpriteVisual MakeSurfaceVisual(wuc::CompositionDrawingSurface const& s, float w, float h);
    wuc::CompositionBrush FileBrush(std::wstring const& path, float logicalPx);
    void LoadFiles();
    void BuildIndex();  // Start-menu apps, once
    void Prefetch();
    void Refresh();
    void Relayout();
    void Place();
    void RunSearch();
    void QueryChanged();
    void SearchDraw();  // search bar text + caret
    void Activate(Kind kind, int index);
    void OnKey(WPARAM vk);
    void Focus();
    void SpringIcon(IconV const& ic, bool up);
    void BounceIcon(IconV const& ic);
    IconV const* IconOf(Hit const& hit) const;
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

    wuc::Compositor m_comp;
    wuc::CompositionGraphicsDevice m_gdev;
    LauncherHost m_host;
    HWND m_hwnd{};
    wuc::Desktop::DesktopWindowTarget m_target{nullptr};
    wuc::ContainerVisual m_root{nullptr};
    wuc::ContainerVisual m_panel{nullptr};
    wuc::ContainerVisual m_shadowHold{nullptr};  // carries the drop shadow, animated with m_panel
    float m_shadowM = 0, m_shadowB = 0;          // shadow margin (px) at the sides / bottom of the window
    winrt::Windows::Foundation::Numerics::float3 PanelHome() const;
    wuc::ContainerVisual m_content{nullptr};  // the panel's contents; dims behind the menu
    wuc::CompositionDrawingSurface m_searchSurface{nullptr}, m_bodySurface{nullptr};
    wuc::SpriteVisual m_caret{nullptr};
    wuc::ContainerVisual m_bodyIcons{nullptr};  // the body's icon sprites, swapped with the body
    wuc::ShapeVisual m_hover{nullptr};
    wuc::CompositionRoundedRectangleGeometry m_hoverShape{nullptr};
    wuc::ContainerVisual m_menu{nullptr};
    wuc::ShapeVisual m_menuHover{nullptr};
    wuc::CompositionRoundedRectangleGeometry m_menuHoverShape{nullptr};
    wuc::CubicBezierEasingFunction m_easeOut{nullptr};

    winrt::com_ptr<IDWriteFactory> m_dwrite;
    std::map<std::wstring, IconImage> m_fileIcons;                 // by extension, or path for per-file icons
    std::map<std::wstring, wuc::CompositionBrush> m_fileBrushes;   // IconKey + px: the icon drawn at cell size

    // Current layout (physical px).
    LauncherAnchor m_anchor;
    float m_s = 1.0f;
    float m_w = 0, m_h = 0;       // panel size
    float m_menuW = 0, m_menuH = 0, m_menuX = 0, m_menuY = 0;
    std::vector<Hit> m_hits, m_menuHits, m_staticHits;
    D2D1_RECT_F m_searchRect{}, m_appsLabel{}, m_filesLabel{}, m_divider{}, m_avatarRect{}, m_nameRect{}, m_settingsRect{}, m_powerRect{};
    float m_bodyTop = 0, m_bodyBottom = 0;  // the body surface spans these panel y's
    std::vector<D2D1_RECT_F> m_appCells, m_fileCells, m_resultRows, m_menuRows;  // icon rects, file cells, result rows, menu rows (menu coordinates)
    std::vector<IconV> m_appSprites, m_fileSprites;
    IconV const* m_hoverIcon = nullptr;
    D2D1_RECT_F m_plateTo{};      // where the hover plate is heading (for TestCommand 20)
    std::vector<std::wstring> m_mru;      // launch targets, newest first
    std::vector<std::wstring> m_apps;     // what the panel shows
    std::vector<FileEntry> m_files;       // the head of m_allFiles the panel shows
    std::vector<FileEntry> m_allFiles;    // the Recent folder, newest first (search looks through all of it)
    std::vector<AppEntry> m_index;        // every Start-menu app
    std::vector<Result> m_results;
    std::wstring m_query;
    int m_sel = 0;                        // selected result
    std::wstring m_user;
    std::vector<uint8_t> m_avatar;        // BGRA premultiplied, m_avatarPx square; empty = draw an initial
    int m_avatarPx = 0;
    bool m_avatarTried = false;
    bool m_indexed = false;
    bool m_dryRun = false;
    float m_iconPx = 44;           // app icon size, logical px

    bool m_open = false;
    bool m_activated = false;     // the panel has had activation since Show, so losing it means a click elsewhere
    bool m_menuOpen = false;
    bool m_tracking = false;
    int m_hoverIndex = -2;        // index into the hits being hovered, -1 none
    bool m_hoverOn = false, m_menuHoverOn = false;  // the hover plates are showing
    int m_pressedIndex = -1;
    unsigned m_gen = 0;           // invalidates a fade-out that a reopen interrupted
    LARGE_INTEGER m_showAt{};     // QPC at Show(), for the open-latency log
};

}  // namespace halo
