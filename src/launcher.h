#pragma once
// The Launcher panel: Start. A glass panel that rises above the dock when the Windows key or the Start
// logo is pressed, and takes the keyboard (it is the only search UI):
//
//   Search bar   - type to filter every app (Start-menu shortcuts and the Apps folder, so Store apps too),
//                  Settings pages and recent files; Enter launches the selected result
//   Recent apps  - up to 6, an MRU kept in %LOCALAPPDATA%\Halo\recent.txt; "All apps" lists every app, A to Z
//   Recent files - 2 x 6 from the shell's Recent folder, newest first
//   user · Settings · Power (Sleep, Restart, Shut down, Lock, Sign out, Use Windows taskbar)
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
#include "services.h"
#include "start_widgets.h"
#include "usage.h"
#include "settings.h"

namespace halo {

namespace wuc = winrt::Windows::UI::Composition;

// What the panel needs from the dock that owns it.
struct LauncherHost {
    // The dock's glass (blur, tint, grain, rim), sized by its parent. The arguments are the grain's height in px and the surface's settings (opacity, tint).
    std::function<wuc::ContainerVisual(int, settings::Surface const&)> glass;
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

    // HALO_TEST: wParam 2 = toggle the Power menu, 3+n = run Power item n (dry run, n 0-5), 9 = pretend the panel
    // lost activation (a click elsewhere), 20 = snap the hover plate and icon to where their animations end,
    // 21 = toggle All apps, 22 = scroll the list down 5 rows.
    void TestCommand(WPARAM w);

    using Found = std::vector<std::pair<std::wstring, std::wstring>>;  // apps found: (name, launch target)

    void Raise();                // back to the top of the topmost band

private:
    enum class Kind { App, File, Result, Settings, Power, MenuItem, AllApps,
                      PowerMode, CcWifi, CcWifiMore, CcBt, CcBtMore, CcBright, CcVol, CcMute, CcPrev, CcPlay, CcNext, CcBack, CcRefresh, CcRow };
    struct Hit {
        D2D1_RECT_F r;
        Kind kind;
        int index;
    };
    struct FileEntry {
        std::wstring name, path;
    };
    struct AppEntry {
        std::wstring name, lname, path;  // path: the Start-menu .lnk or shell:AppsFolder\<id>, which the host launches
    };
    struct Result {
        enum class Type { App, Setting, File } type;  // also the order among equally good matches
        std::wstring name, path;                     // path: a launch target, an ms-settings: URI or a file
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
    void DrawList(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawMenu(ID2D1DeviceContext* dc, float ox, float oy);
    void Redraw(wuc::CompositionDrawingSurface const& s, void (Launcher::*draw)(ID2D1DeviceContext*, float, float));
    wuc::CompositionDrawingSurface MakeSurface(float w, float h);
    wuc::SpriteVisual MakeSurfaceVisual(wuc::CompositionDrawingSurface const& s, float w, float h);
    wuc::CompositionBrush FileBrush(std::wstring const& path, float logicalPx);
    void LoadFiles();
    void BuildIndex();  // every app, once (the Apps folder arrives later, on kMsgAppsFolder)
    void AddToIndex(Found const& found);
    void Prefetch();
    void Refresh();
    void Relayout();
    void Place();
    void RunSearch();
    void ShowMatches();  // m_results = the rows of m_matches that fit, from m_scroll
    void Scroll(int rows);
    void ToggleAllApps();
    bool Listing() const { return !m_query.empty() || m_allApps; }
    bool ListHeader() const { return m_allApps && m_query.empty(); }
    float ListTop() const;
    int ListRows() const;
    void LayoutRight();  // the right column's hit rects for the current state (home or a radio list)
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
    void ShowDim(bool on);

    // ---- HALO 2.0 (settings.halo2): the wider two-column panel. Left = Start plus a widget strip; right = a compact
    // control centre on the same services the Control Center uses. With halo2 off none of this runs. ----
    enum class Detail { None, Wifi, Bt };
    float PanelX() const;                  // the panel's left edge in screen px (halo2 sits on the left of the screen)
    float CornerX() const;                 // where along the panel the swoop grows from (the dock is under it)
    void BuildRight(float rightW);         // the right column's visuals, hit rects and first paint
    void ScrollHome(float delta);
    void DrawHeader(ID2D1DeviceContext* dc, float ox, float oy);
    void DrawWidgets(ID2D1DeviceContext* dc, float ox, float oy);  // greeting, quote, clock, month, battery, power mode
    void DrawRight(ID2D1DeviceContext* dc, float ox, float oy);    // tiles, sliders, now playing / the radio list
    void DrawUsage(ID2D1DeviceContext* dc, float ox, float oy);    // the usage monitor
    void RedrawRight();
    void RedrawUsage();
    void RedrawBody();
    void StartServices();                  // on Show: bind audio, radios, media, brightness; start the 1 Hz sampler
    void StopServices();                   // on Close: nothing runs while Start is closed
    void RefreshWidgets();                 // time, battery, power mode; re-armed once a minute while open
    void ArmClock();
    void CycleMode();
    void ToggleDetail(Detail d);
    void CcClick(Hit const& hit);
    void CcDrag(float x);                  // slider drag (panel coords)
    void CcKey(WPARAM vk);                 // keys while the right column has focus
    void SetCol(int col);
    Hit const* CcHitAt(float x, float y) const;
    int CcIndexOf(Kind kind, int index) const;
    void OnServiceMessage(UINT m);

    bool m_h2 = false;
    float m_lw = 0;                        // the left column's width (== m_w without halo2)
    start::LeftPlan m_plan;
    start::RightPlan m_rplan;
    D2D1_RECT_F m_greetRect{}, m_cardRect{}, m_calRect{}, m_modeRect{}, m_rightRect{};
    float m_rightW = 0;
    svc::Battery m_battery;
    svc::PowerMode m_mode = svc::PowerMode::Unknown;
    svc::AudioEndpoint m_speaker;
    svc::Radios m_radios;
    svc::Media m_media;
    usage::Sampler m_sampler;
    usage::Sample m_use;
    bool m_wifiOn = false, m_btOn = false, m_muted = false, m_servicesOn = false;
    std::wstring m_ssid;
    float m_vol = 0;
    int m_bright = -1;
    Detail m_detail = Detail::None;
    std::vector<svc::WifiNet> m_wifiRows;
    std::vector<svc::BtDevice> m_btRows;
    int m_ccScroll = 0;                    // first visible row of the radio list
    int m_ccRowsFit = 0;
    std::vector<Hit> m_ccHits;             // the right column's controls (panel coords), in tab order
    wuc::CompositionDrawingSurface m_ccSurface{nullptr}, m_usageSurface{nullptr};
    wuc::SpriteVisual m_usageVisual{nullptr};
    D2D1_RECT_F m_usageRect{};             // panel coords
    int m_ccHover = -1, m_ccFocus = -1;    // indices into m_ccHits
    int m_col = 0;                         // 0 = left (search), 1 = right (controls); Tab switches
    int m_drag = 0;                        // 0 none, else the Kind of the slider being dragged
    ULONGLONG m_lastBrightSet = 0;

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
    wuc::CompositionDrawingSurface m_searchSurface{nullptr}, m_bodySurface{nullptr}, m_headerSurface{nullptr};
    float m_homeScroll = 0;
    wuc::SpriteVisual m_caret{nullptr};
    wuc::ContainerVisual m_bodyIcons{nullptr};  // the body's icon sprites, swapped with the body
    // The result rows: their own surface and icons, drawn a few rows past each end of what shows, in a holder that
    // a scroll slides from where the rows were to where they are now (m_listClip cuts it to the list).
    wuc::CompositionDrawingSurface m_listSurface{nullptr};
    wuc::ContainerVisual m_listClip{nullptr}, m_listHold{nullptr}, m_listIcons{nullptr};
    wuc::InsetClip m_listInset{nullptr};
    float m_scrollFrom = 0;           // the slide's start offset (px)
    LARGE_INTEGER m_scrollAt{};       // QPC when it started
    float ScrollLeft() const;         // how far the running slide still has to go (px)
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
    D2D1_RECT_F m_allAppsRect{};  // "All apps" on the home view, "Back" on the list: the same place
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
    std::vector<Result> m_matches;        // everything the query (or All apps) found, best first
    std::vector<Result> m_results;        // the rows of it on screen
    int m_scroll = 0;                     // index in m_matches of the first row
    int m_wheel = 0;                      // wheel delta not yet a whole row
    bool m_allApps = false;
    std::wstring m_query;
    int m_sel = 0;                        // selected row of m_results
    std::wstring m_user;
    std::vector<uint8_t> m_avatar;        // BGRA premultiplied, m_avatarPx square; empty = draw an initial
    int m_avatarPx = 0;
    bool m_avatarTried = false;
    bool m_indexed = false;
    bool m_dryRun = false;
    float m_iconPx = 44;           // app icon size, logical px

    HWND m_dim{};                 // [start] dim_desktop: the click-through sheet under the panel
    bool m_showApps = true, m_showFiles = true;  // [start] recent_apps / recent_files are not 0
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
