// Halo M0 spike: a macOS-style dock drawn with Windows.UI.Composition in a
// transparent, topmost, non-activating Win32 window: a frosted-glass pill
// with the user's pinned apps, magnified on hover by the compositor.
#include <windows.h>
#include <shellscalingapi.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi.h>
#include <dispatcherqueue.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <algorithm>
#include <tuple>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <map>
#include <memory>
#include <shellapi.h>
#include <string>
#include <utility>
#include <vector>
#include "apps.h"
#include "cluster.h"
#include "control_center.h"
#include "notifications.h"
#include "theme.h"
#include "dock.h"
#include "effects.h"
#include "icons.h"
#include "intellihide.h"
#include "launcher.h"
#include "label.h"
#include "multimon.h"
#include "previews.h"
#include "menu.h"
#include "pins.h"
#include "taskbar.h"
#include "winkey.h"
#include "edge.h"

// Interop interfaces missing from the MinGW SDK headers (layouts per the Windows SDK).
struct ICompositorDesktopInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopWindowTarget(HWND hwnd, BOOL isTopmost, void** target) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnsureOnThread(DWORD threadId) = 0;
};
__CRT_UUID_DECL(ICompositorDesktopInterop, 0x29E691FA, 0x4567, 0x4DCA, 0xB3, 0x19, 0xD0, 0xF2, 0x07, 0xEB, 0x68, 0x07)
#ifndef HALO_COMPOSITION_INTEROP_DEFINED
#define HALO_COMPOSITION_INTEROP_DEFINED
struct ICompositionDrawingSurfaceInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginDraw(RECT const* updateRect, REFIID iid, void** updateObject, POINT* offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resize(SIZE size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Scroll(RECT const* scrollRect, RECT const* clipRect, int dx, int dy) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendDraw() = 0;
};
__CRT_UUID_DECL(ICompositionDrawingSurfaceInterop, 0xFD04E6E3, 0xFE0C, 0x4C3C, 0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE)
#endif
extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Size;
using winrt::Windows::Foundation::Numerics::float2;

namespace {

// Motion defaults; [taskbar] speed / motion / effect / hide_delay in settings.ini scale or replace them.
constexpr float kSpringDamping = 0.72f;   // < 1 overshoots slightly
constexpr int kSpringPeriodMs = 55;       // lower = snappier
constexpr int kHideDurationMs = 200;
constexpr int kDockFadeMs = 180;          // the fade of the non-rise reveal effects
constexpr float kDockTravel = 24;         // logical px a swipe / swoop reveal moves
constexpr float kLabelRoom = 34;  // logical px above a magnified icon: a 6 px gap and the name label
constexpr int kStripPx = 2;               // hit strip left on the screen edge while hidden
constexpr size_t kMenuWindows = 10;       // windows listed in an icon's right-click menu; the rest are counted

constexpr UINT kMsgAppsChanged = WM_APP + 1;  // from the window tracker
// Test hook (HALO_TEST=1): simulate the pointer without touching the real one.
constexpr UINT kMsgTestMove = WM_APP + 2;   // wParam = pointer x, client px
constexpr UINT kMsgTestLeave = WM_APP + 3;
constexpr UINT kMsgIntellihideChanged = WM_APP + 5;  // wParam = overlap, lParam = fullscreen
constexpr UINT kMsgMonitorSwitch = WM_APP + 6;       // wParam = HMONITOR
// Test hook (HALO_TEST=1): wParam 0 = open the launcher panel, 1 = close it, else Launcher::TestCommand.
constexpr UINT kMsgTestLauncher = WM_APP + 8;
constexpr UINT kMsgTestControlCenter = WM_APP + 20;  // HALO_TEST=1: toggle the Control Center
constexpr UINT kMsgSettings = WM_APP + 50;  // halo::settings::Watch: settings.ini changed
constexpr UINT kMsgGuardianGone = WM_APP + 60;  // the guardian that started us exited
constexpr UINT kMsgEdge = WM_APP + 40;  // halo::edge gesture: wParam kTop (Control Center) / kRight (notifications)
constexpr UINT kMsgTestNotifCenter = WM_APP + 21;  // HALO_TEST=1: toggle the Notification Center
constexpr UINT kMsgTestMenu = WM_APP + 31;    // HALO_TEST=1: wParam = dock item; lParam 0 = open its menu, 1 = close, 100+r = hover row r, 200+r = choose row r
constexpr UINT kMsgTestBounce = WM_APP + 30;  // HALO_TEST=1: wParam = dock item to bounce (no launch)
constexpr UINT kGenieHoldMs = 620;  // dock stays up for the 320 ms scale effect + 300 ms (mods/halo-genie.wh.cpp)
constexpr int kHotkeyQuit = 1;
constexpr UINT_PTR kHideTimer = 1;
constexpr UINT_PTR kPreviewTimer = 21;  // the pointer rested on a running app
constexpr UINT_PTR kPreviewGrace = 22;  // the pointer left the icon and the strip
constexpr UINT_PTR kPanelStagger = 23;  // the notification column follows the Control Center
constexpr UINT kPanelStaggerMs = 40;
constexpr UINT kPreviewDelayMs = 400;
constexpr UINT kPreviewGraceMs = 300;
constexpr DWORD kDwmUseHostBackdropBrush = 17;  // DWMWA_USE_HOSTBACKDROPBRUSH

void Log(std::wstring const& msg) {
    auto line = L"[halo] " + msg + L"\n";
    OutputDebugStringW(line.c_str());
    fputws(line.c_str(), stderr);
    fflush(stderr);
}

std::wstring Hex(HRESULT hr) {
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", static_cast<unsigned>(hr));
    return buf;
}

// Runs a setup step; failure is logged, not fatal.
template <typename F>
bool Step(wchar_t const* name, F&& f) {
    try {
        f();
        Log(std::wstring(name) + L": ok");
        return true;
    } catch (winrt::hresult_error const& e) {
        Log(std::wstring(name) + L": FAILED " + Hex(e.code()) + L" " + e.message().c_str());
    } catch (std::exception const&) {
        Log(std::wstring(name) + L": FAILED (std::exception)");
    }
    return false;
}

struct Scale {
    float v;
    int px(float logical) const { return static_cast<int>(logical * v + 0.5f); }
    float f(float logical) const { return logical * v; }
};

// guardian.exe starts us with "--guardian <its pid>" and is what puts the taskbar back whenever we go away.
// If it dies while we run (killed, crashed) nobody would. The thread pool waits on its process handle (no
// timer, no polling) and posts kMsgGuardianGone; Halo then starts a new guardian with "--adopt <our pid>".
HWND g_guardianNotify = nullptr;
bool g_unguarded = false;  // the guardian is gone and no replacement started: restore the taskbar on exit

void CALLBACK GuardianGoneCb(PVOID, BOOLEAN) {
    if (g_guardianNotify) PostMessageW(g_guardianNotify, kMsgGuardianGone, 0, 0);
}

void WatchGuardian(DWORD pid, HWND hwnd) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) {
        Log(L"guardian " + std::to_wstring(pid) + L" already gone");
        PostMessageW(hwnd, kMsgGuardianGone, 0, 0);
        return;
    }
    g_guardianNotify = hwnd;
    HANDLE wait = nullptr;
    if (!RegisterWaitForSingleObject(&wait, h, GuardianGoneCb, nullptr, INFINITE, WT_EXECUTEONLYONCE))
        Log(L"RegisterWait on the guardian failed " + std::to_wstring(GetLastError()));
    // h and wait stay open for the life of the process.
}

void StartAdopter() {
    static int tries = 0;
    if (++tries > 3) {  // adopters that keep exiting at once (another guardian owns the mutex): stop respawning
        g_unguarded = true;
        Log(L"giving up on a replacement guardian");
        return;
    }
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    std::wstring dir(buf, n);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    std::wstring exe = dir + L"guardian.exe";
    std::wstring cmd = L"\"" + exe + L"\" --adopt " + std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Log(L"guardian gone; started a new one to adopt us");
        WatchGuardian(pi.dwProcessId, g_guardianNotify);  // and keep watching: the new one can die too
    } else {
        g_unguarded = true;
        Log(L"guardian gone and guardian.exe would not start " + std::to_wstring(GetLastError()));
    }
}

class Halo {
public:
    ~Halo();
    bool Create(HINSTANCE inst);
    int Run();
    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT, WPARAM, LPARAM);
    void InitComposition();
    void InitGraphics();
    wuc::ContainerVisual BuildPill(int noiseHeight = 0, float radius = 0.0f, halo::settings::Surface const* glass = nullptr);
    void ComputeMetrics();
    void ApplySettings(bool themeFlipped = false);
    void RebuildDock(bool keepIcons = false);
    wuc::SpriteVisual BuildPillShadow(wuc::ContainerVisual const& pill);
    wuc::CompositionBrush MakeNoiseBrush(int w, int h);
    void BuildDock();
    void RefreshDock();
    void UpdateRegion();
    wuc::CompositionBrush IconBrushFor(std::wstring const& path);
    wuc::CompositionBrush FallbackIconBrush();
    void Activate(size_t item);
    void ActivateWindow(HWND w);
    void ShowPreviewsFor(int item);
    void RefreshPreviews();
    void HidePreviews(bool instant = false);
    void OpenMenuFor(int item);
    void ReloadPins();
    void PointerMove(float x, float y = 0.0f);
    void PointerLeave();
    void Reveal();
    void OpenLauncher();
    // Control Center + notifications + calendar are one column on the primary monitor, opened, toggled and
    // closed together: Win+A, Win+N, the edge gestures and the test messages all come through here.
    void OpenPanel(bool byPointer = false, bool withNotifs = true);
    void ClosePanel();
    void TogglePanel() { if (m_cc && m_cc->IsOpen()) ClosePanel(); else OpenPanel(); }
    halo::LauncherAnchor LauncherAnchorNow() const;
    std::wstring LaunchTarget(RunningApp const& app) const;
    void LaunchRecent(std::wstring const& target);
    LRESULT DockIconForWindow(HWND win, LPARAM op);
    void StartHide();
    RECT GetPillRectScreen() const;
    void MigrateToMonitor(HMONITOR mon, bool reveal = true);
    wuc::CompositionDrawingSurface MakeSurface(Size size);
    wuc::CompositionBrush MakeIconBrush(IconImage const& icon, float size);

    HWND m_hwnd{};
    Scale m_scale{1.0f};
    int m_width{}, m_height{};
    DockMetrics m_metrics;
    std::vector<PinnedApp> m_apps;
    struct Item {
        std::wstring launch;  // .lnk to open when the app isn't running (empty for unpinned apps)
        std::wstring key;     // the running app's key, empty if not running
        std::wstring pinPath; // what Keep in Dock would store for an unpinned app
        std::wstring file;    // the exe or shortcut target Show in Explorer selects; empty for Store apps
    };
    std::vector<Item> m_items;  // what the dock shows, left to right
    std::vector<std::wstring> m_appNames;  // friendly display names for hover labels
    std::map<std::wstring, wuc::CompositionBrush> m_brushes;  // by icon source path, so rebuilds reuse them
    std::unique_ptr<WindowTracker> m_tracker;
    std::unique_ptr<Dock> m_dock;
    std::unique_ptr<DockCluster> m_cluster;
    std::unique_ptr<halo::AppLabel> m_label;
    std::unique_ptr<halo::Launcher> m_launcher;
    std::unique_ptr<halo::Previews> m_previews;
    std::unique_ptr<halo::Menu> m_menu;
    int m_hoverItem = -1;           // the dock icon the pointer is on
    std::wstring m_previewKey;      // the app the strip shows
    std::unique_ptr<halo::IntellihideDetector> m_intellihide;
    std::unique_ptr<halo::MultiMonitorFollower> m_multimon;
    std::unique_ptr<halo::NotificationCenter> m_notif;
    std::unique_ptr<halo::ControlCenter> m_cc;
    bool m_trackingMouse = false;
    int m_pressed = -1;  // the icon the mouse button went down on
    bool m_pressedStart = false;  // the button went down on the Start logo
    bool m_autoHide = true;  // off under HALO_TEST
    halo::settings::Settings m_cfg;  // what the dock was last built / applied with
    UINT m_genieMsg = RegisterWindowMessageW(L"Halo.DockIconForWindow");

    enum class State { Hidden, Shown, Hiding };
    State m_state = State::Shown;
    unsigned m_hideGen = 0;  // invalidates a hide batch that a reveal interrupted
    HMONITOR m_currentMon = nullptr;
    RECT m_monitor{}, m_full{};
    wuc::ContainerVisual m_slide{nullptr};  // the dock's parent; its Offset.Y slides the dock off-screen

    winrt::Windows::System::DispatcherQueueController m_dq{nullptr};
    wuc::Compositor m_comp{nullptr};
    wuc::Desktop::DesktopWindowTarget m_target{nullptr};
    wuc::ContainerVisual m_root{nullptr};

    winrt::com_ptr<ID3D11Device> m_d3d;
    winrt::com_ptr<ID2D1Device> m_d2d;
    wuc::CompositionGraphicsDevice m_gdev{nullptr};
};

// Members die in reverse declaration order, so m_comp/m_dq (declared last) are gone before
// m_label's destructor calls m_parent.Children().Remove() on a visual of the dead compositor, which
// throws winrt::hresult_error out of a noexcept destructor -> terminate on every exit. Drop the
// visual owners while the compositor is still alive.
Halo::~Halo() {
    m_label.reset();
}

bool Halo::Create(HINSTANCE inst) {
    // user32 has it from Windows 10 1703; older builds keep the manifest default (a failed call is harmless).
    if (auto setDpi = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")))
        setDpi(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    halo::theme::Refresh();
    m_cfg = halo::settings::Get();
    m_autoHide = !_wgetenv(L"HALO_TEST") && m_cfg.taskbar.autohide;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloDockWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) return false;

    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    m_currentMon = mon;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    m_scale.v = dpiX / 96.0f;

    Step(L"pinned apps", [&] { m_apps = halo::pins::Load(); });

    // Metrics in physical px. The window is as wide as the monitor and tall
    // enough for a fully magnified, bouncing icon above the pill; only the
    // dock's part of it takes input (UpdateRegion), so it never has to resize
    // (unless the icon size or magnification setting changes).
    m_monitor = mi.rcMonitor;
    m_width = m_monitor.right - m_monitor.left;
    ComputeMetrics();
    int x = m_monitor.left, y = m_monitor.bottom - m_height;
    m_full = {x, y, x + m_width, y + m_height};
    if (m_autoHide) m_state = State::Hidden;

    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Halo", WS_POPUP, x, y, m_width, m_height, nullptr, nullptr, inst,
                             this);
    if (!m_hwnd) {
        Log(L"CreateWindowEx failed " + std::to_wstring(GetLastError()));
        return false;
    }
    Log(L"window " + std::to_wstring(m_width) + L"x" + std::to_wstring(m_height) + L" at " + std::to_wstring(x) +
        L"," + std::to_wstring(y) + L" scale " + std::to_wstring(m_scale.v) + L", " +
        std::to_wstring(m_apps.size()) + L" apps");

    BOOL on = TRUE;
    HRESULT hr = DwmSetWindowAttribute(m_hwnd, kDwmUseHostBackdropBrush, &on, sizeof(on));
    Log(L"DWMWA_USE_HOSTBACKDROPBRUSH hr=" + Hex(hr));

    bool ok = Step(L"composition", [&] { InitComposition(); });
    if (ok) {
        Step(L"graphics device", [&] { InitGraphics(); });
        ok = Step(L"dock", [&] { BuildDock(); });
        if (ok) {
            Step(L"app label", [&] {
                m_label = std::make_unique<halo::AppLabel>(m_comp, m_slide, m_gdev, m_scale.v);
            });
            Step(L"intellihide", [&] {
                m_intellihide = std::make_unique<halo::IntellihideDetector>(m_hwnd, kMsgIntellihideChanged);
                m_intellihide->SetMonitor(m_currentMon, m_monitor);
                m_intellihide->SetDockRect(GetPillRectScreen());
            });
            Step(L"multi-monitor", [&] {
                m_multimon = std::make_unique<halo::MultiMonitorFollower>(m_hwnd, kMsgMonitorSwitch);
                m_multimon->SetCurrentMonitor(m_currentMon);
            });
            Step(L"launcher", [&] {
                halo::LauncherHost host;
                host.glass = [this](int noiseHeight, halo::settings::Surface const& g) { return BuildPill(noiseHeight, 0.0f, &g); };
                host.appIcon = [this](std::wstring const& target) { return IconBrushFor(target); };
                host.launch = [this](std::wstring const& target) { LaunchRecent(target); };
                host.closed = [this] { if (!m_trackingMouse) PointerLeave(); };  // the dock may auto-hide again
                m_launcher = std::make_unique<halo::Launcher>(inst, m_comp, m_gdev, std::move(host));
                m_tracker->onForeground = [this](RunningApp const& app) { m_launcher->Note(LaunchTarget(app)); };
            });
            Step(L"menu", [&] {
                halo::MenuHost host;
                host.glass = [this](int noiseHeight, float radius) { return BuildPill(noiseHeight, radius); };
                host.opened = [] { halo::winkey::WatchEscape(true); };  // the hook posts Esc to us
                host.closed = [this] {
                    if (!(m_launcher && m_launcher->IsOpen())) halo::winkey::WatchEscape(false);
                    if (!m_trackingMouse) PointerLeave();  // the dock may auto-hide again
                };
                m_menu = std::make_unique<halo::Menu>(inst, m_comp, m_gdev, std::move(host));
            });
            Step(L"previews", [&] {
                halo::PreviewsHost host;
                host.glass = [this](int noiseHeight) { return BuildPill(noiseHeight); };
                host.activate = [this](HWND w) { ActivateWindow(w); };
                host.entered = [this] { KillTimer(m_hwnd, kPreviewGrace); Reveal(); };
                host.left = [this] { SetTimer(m_hwnd, kPreviewGrace, kPreviewGraceMs, nullptr); };
                host.refresh = [this] { RefreshPreviews(); };
                m_previews = std::make_unique<halo::Previews>(inst, m_comp, m_gdev, std::move(host));
            });
        }
    }
    if (!RegisterHotKey(m_hwnd, kHotkeyQuit, MOD_CONTROL | MOD_WIN | MOD_NOREPEAT, VK_ESCAPE))
        Log(L"RegisterHotKey Ctrl+Win+Esc failed " + std::to_wstring(GetLastError()));
    Step(L"control center", [&] { m_cc = std::make_unique<halo::ControlCenter>(inst, m_hwnd, m_comp, m_gdev); });
    Step(L"notifications", [&] {
        m_notif = std::make_unique<halo::NotificationCenter>(inst, m_hwnd, m_comp, m_gdev);
    });
    if (m_cc && m_notif) {
        // Two windows, one column: focus moving between them must not close anything; Esc or a click
        // outside closes both; the notification column tracks the Control Center's size.
        m_cc->siblingHwnd = [this] { return m_notif ? m_notif->Hwnd() : nullptr; };
        m_notif->siblingHwnd = [this] { return m_cc ? m_cc->Hwnd() : nullptr; };
        m_notif->onDismiss = [this] { ClosePanel(); };
        m_cc->onHidden = [this] {
            KillTimer(m_hwnd, kPanelStagger);
            if (m_notif) m_notif->Close();
            Log(L"panel: closed");
        };
        m_cc->onResized = [this] { if (m_notif && m_notif->IsOpen()) m_notif->SetAnchor(m_cc->PanelRectScreen()); };
        m_cc->onClock = [this] {  // the calendar and notifications rise out of the panel, or go
            if (m_notif->IsOpen()) m_notif->Close();
            else OpenPanel(false, true);
        };
    }
    if (_wgetenv(L"HALO_TEST_NOHOOK")) {}  // a test instance must not fight the live dock for Win / Win+A
    else if (!halo::winkey::Install(m_hwnd)) Log(L"Win-key hook failed " + std::to_wstring(GetLastError()));
    halo::edge::Configure(m_cfg.cc.hotCorner, m_cfg.cc.cornerWidth, m_cfg.cc.cornerDelay);
    halo::settings::Watch(m_hwnd, kMsgSettings);  // edits to settings.ini apply live (ApplySettings)
    if (!_wgetenv(L"HALO_TEST_NOHOOK") && !halo::edge::Start(inst, m_hwnd, kMsgEdge))
        Log(L"edge mouse hook failed " + std::to_wstring(GetLastError()));

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    return ok;
}

void Halo::InitComposition() {
    DispatcherQueueOptions opt{sizeof(opt), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
    winrt::check_hresult(CreateDispatcherQueueController(
        opt, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(m_dq))));

    m_comp = wuc::Compositor();
    auto interop = m_comp.as<ICompositorDesktopInterop>();
    winrt::check_hresult(interop->CreateDesktopWindowTarget(m_hwnd, TRUE, winrt::put_abi(m_target)));
    m_root = m_comp.CreateContainerVisual();
    m_root.RelativeSizeAdjustment({1, 1});
    m_target.Root(m_root);
    m_slide = m_comp.CreateContainerVisual();
    m_slide.RelativeSizeAdjustment({1, 1});
    if (m_autoHide) m_slide.Offset({0, static_cast<float>(m_height), 0});
    m_root.Children().InsertAtTop(m_slide);
}

void Halo::InitGraphics() {
    // Our device only draws the icons once; the compositor renders everything
    // on the GPU regardless. The software (WARP) device avoids loading the GPU
    // driver into this process: measured 17 MB private vs 46 MB on hardware.
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                   0, D3D11_SDK_VERSION, m_d3d.put(), nullptr, nullptr);
    if (FAILED(hr)) {
        Log(L"WARP device unavailable " + Hex(hr) + L", using the GPU");
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                               m_d3d.put(), nullptr, nullptr));
    }
    auto dxgi = m_d3d.as<IDXGIDevice>();
    winrt::check_hresult(D2D1CreateDevice(dxgi.get(), nullptr, m_d2d.put()));

    auto interop = m_comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
    winrt::check_hresult(interop->CreateGraphicsDevice(
        reinterpret_cast<IUnknown*>(m_d2d.get()),
        reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(m_gdev))));
}

wuc::CompositionDrawingSurface Halo::MakeSurface(Size size) {
    return m_gdev.CreateDrawingSurface(size, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                       winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
}

// Draws the 256px icon into a surface the size of the fully magnified icon,
// with high-quality downscaling: magnified icons show 1:1 pixels, and at
// rest the compositor only shrinks it by 1/maxScale, so nothing shimmers.
wuc::CompositionBrush Halo::MakeIconBrush(IconImage const& icon, float size) {
    float px = std::ceil(size);
    auto surface = MakeSurface({px, px});
    auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset));
    try {
        dc->Clear(D2D1::ColorF(0, 0.0f));
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(icon.width, icon.height), icon.bgra.data(),
                                              icon.width * 4, &props, bitmap.put()));
        D2D1_RECT_F dest = D2D1::RectF(static_cast<float>(offset.x), static_cast<float>(offset.y),
                                       offset.x + px, offset.y + px);
        dc->DrawBitmap(bitmap.get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    } catch (...) {
        interop->EndDraw();
        throw;
    }
    winrt::check_hresult(interop->EndDraw());
    auto brush = m_comp.CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::Uniform);
    return brush;
}

// Grayscale noise, drawn once, for the glass's grain.
wuc::CompositionBrush Halo::MakeNoiseBrush(int w, int h) {
    auto surface = MakeSurface({static_cast<float>(w), static_cast<float>(h)});
    auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset));
    try {
        std::vector<uint32_t> px(static_cast<size_t>(w) * h);
        uint32_t seed = 0x2545F491;
        for (auto& p : px) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;  // xorshift32
            uint32_t g = seed & 0xFF;
            p = 0xFF000000 | g << 16 | g << 8 | g;
        }
        winrt::com_ptr<ID2D1Bitmap> bitmap;
        D2D1_BITMAP_PROPERTIES props =
            D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &props, bitmap.put()));
        D2D1_RECT_F dest = D2D1::RectF(static_cast<float>(offset.x), static_cast<float>(offset.y),
                                       static_cast<float>(offset.x + w), static_cast<float>(offset.y + h));
        dc->DrawBitmap(bitmap.get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
    } catch (...) {
        interop->EndDraw();
        throw;
    }
    winrt::check_hresult(interop->EndDraw());
    auto brush = m_comp.CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::None);
    return brush;
}

// Quiet Dock's glass: blurred host backdrop, a #16161A tint at 50%, 2.5%
// grain and a 1px rim whose vertical gradient is bright on top and faintly
// bright again at the bottom. Layers size relative to the container, which
// the dock resizes and clips.
wuc::ContainerVisual Halo::BuildPill(int noiseHeight, float radius, halo::settings::Surface const* glassCfg) {
    auto pill = m_comp.CreateContainerVisual();

    auto glass = m_comp.CreateSpriteVisual();
    glass.RelativeSizeAdjustment({1, 1});
    glass.Brush(m_comp.CreateHostBackdropBrush());  // already blurred by DWM (see control_center.cpp)
    pill.Children().InsertAtTop(glass);

    auto tint = m_comp.CreateSpriteVisual();
    tint.RelativeSizeAdjustment({1, 1});
    tint.Brush(m_comp.CreateColorBrush(halo::theme::C(halo::theme::Glass(glassCfg ? *glassCfg : halo::settings::Get().taskbar).tint)));
    pill.Children().InsertAtTop(tint);

    Step(L"noise", [&] {
        auto noise = m_comp.CreateSpriteVisual();
        noise.RelativeSizeAdjustment({1, 1});
        noise.Opacity(halo::theme::P().noise);
        noise.Brush(MakeNoiseBrush(m_monitor.right - m_monitor.left, noiseHeight ? noiseHeight : static_cast<int>(std::ceil(m_metrics.pillHeight))));
        pill.Children().InsertAtTop(noise);
    });

    // The rim is a stroke inset by half its width, so it stays inside the clip.
    const float t = m_scale.f(1);
    const float r = (radius > 0 ? radius : m_metrics.cornerRadius) - t / 2;
    auto rimShape = m_comp.CreateRoundedRectangleGeometry();
    const bool hasEdge = halo::theme::P().edge.a != 0;
    const float ri = hasEdge ? t : 0.0f;  // the white rim sits just inside the outer hairline
    rimShape.Offset({t / 2 + ri, t / 2 + ri});
    rimShape.CornerRadius({r - ri, r - ri});
    auto rimSize = m_comp.CreateExpressionAnimation(
        L"Vector2(pill.Size.X - " + std::to_wstring(t + 2 * ri) + L", pill.Size.Y - " + std::to_wstring(t + 2 * ri) + L")");
    rimSize.SetReferenceParameter(L"pill", pill);
    rimShape.StartAnimation(L"Size", rimSize);
    auto edgeSize = m_comp.CreateExpressionAnimation(
        L"Vector2(pill.Size.X - " + std::to_wstring(t) + L", pill.Size.Y - " + std::to_wstring(t) + L")");
    edgeSize.SetReferenceParameter(L"pill", pill);
    auto gradient = m_comp.CreateLinearGradientBrush();
    gradient.MappingMode(wuc::CompositionMappingMode::Relative);
    gradient.StartPoint({0, 0});
    gradient.EndPoint({0, 1});
    auto const& pal = halo::theme::P();
    auto stop = [&](float at, halo::theme::Argb c) {
        gradient.ColorStops().Append(m_comp.CreateColorGradientStop(at, halo::theme::C(c)));
    };
    stop(0.0f, pal.rimTop); stop(0.35f, pal.rimMid); stop(0.7f, pal.rimLow); stop(1.0f, pal.rimBottom);
    auto rimSprite = m_comp.CreateSpriteShape(rimShape);
    rimSprite.StrokeBrush(gradient);
    rimSprite.StrokeThickness(t);
    auto rim = m_comp.CreateShapeVisual();
    rim.RelativeSizeAdjustment({1, 1});
    rim.Shapes().Append(rimSprite);
    pill.Children().InsertAtTop(rim);

    // Outer hairline: a second stroke of the same rounded rect, flush with the clip edge,
    // so glass reads against a light wallpaper.
    if (pal.edge.a) {
        auto edgeShape = m_comp.CreateRoundedRectangleGeometry();
        edgeShape.Offset({t / 2, t / 2});
        edgeShape.CornerRadius({r, r});
        edgeShape.StartAnimation(L"Size", edgeSize);
        auto edgeSprite = m_comp.CreateSpriteShape(edgeShape);
        edgeSprite.StrokeBrush(m_comp.CreateColorBrush(halo::theme::C(pal.edge)));
        edgeSprite.StrokeThickness(t);
        auto edge = m_comp.CreateShapeVisual();
        edge.RelativeSizeAdjustment({1, 1});
        edge.Shapes().Append(edgeSprite);
        pill.Children().InsertAtTop(edge);
    }
    return pill;  // the dock places it in the tree
}

// settings.ini changed (halo::settings::Reload() found a difference): apply it without a restart. Motion
// settings are read where they are used; glass, size and layout changes close the panel they touch, which
// rebuilds on its next open, and the dock rebuilds its pill and icons in place.
void Halo::ApplySettings(bool themeFlipped) {
    using halo::settings::Surface;
    const auto prev = m_cfg;
    halo::theme::Refresh();
    m_cfg = halo::settings::Get();
    auto const& c = m_cfg;
    auto glass = [](Surface const& g) { return std::make_tuple(g.opacity, g.blur, g.hasTint, g.tint); };
    const bool look = themeFlipped || prev.theme != c.theme || prev.accent != c.accent;
    const bool dockChanged = look || glass(prev.taskbar) != glass(c.taskbar) || prev.reduceMotion != c.reduceMotion ||
                             prev.taskbar.iconSize != c.taskbar.iconSize || prev.taskbar.magnification != c.taskbar.magnification ||
                             prev.taskbar.runningDots != c.taskbar.runningDots;
    const bool ccChanged = look || glass(prev.cc) != glass(c.cc) || prev.cc.showMedia != c.cc.showMedia ||
                           prev.cc.showSliders != c.cc.showSliders;
    const bool startChanged = look || glass(prev.start) != glass(c.start) || prev.start.recentApps != c.start.recentApps ||
                              prev.start.recentFiles != c.start.recentFiles || prev.start.dimDesktop != c.start.dimDesktop;
    Log(L"settings: changed, applying (dock " + std::to_wstring(dockChanged) + L", control center " +
        std::to_wstring(ccChanged) + L", start " + std::to_wstring(startChanged) + L"; accent #" + [&] {
            wchar_t hex[8];
            swprintf(hex, 8, L"%06X", c.accent);
            return std::wstring(hex);
        }() + L")");

    halo::edge::Configure(c.cc.hotCorner, c.cc.cornerWidth, c.cc.cornerDelay);
    m_autoHide = !_wgetenv(L"HALO_TEST") && c.taskbar.autohide;
    if (!c.taskbar.autohide) Reveal();
    else if (!prev.taskbar.autohide && !m_trackingMouse) PointerLeave();
    if (startChanged && m_launcher && m_launcher->IsOpen()) m_launcher->Hide();  // it is built per open
    if (ccChanged && m_cc) {
        if (m_notif && m_notif->IsOpen()) ClosePanel();
        m_cc->OnSettingsChanged();
    }
    if (dockChanged) RebuildDock();
}

// The pill, icons, cluster and label are rebuilt with the current settings; the window resizes if the icon
// size or magnification moved its height. keepIcons: the icon surfaces are still the right size and look.
void Halo::RebuildDock(bool keepIcons) {
    if (!m_dock) return;
    HidePreviews(true);
    if (m_menu && m_menu->IsOpen()) m_menu->Close();
    KillTimer(m_hwnd, kPreviewTimer);
    KillTimer(m_hwnd, kPreviewGrace);
    m_hoverItem = -1;
    m_pressed = -1;
    m_label.reset();
    m_dock->Detach();
    m_dock.reset();
    m_cluster.reset();
    if (!keepIcons) m_brushes.clear();  // icon surfaces are sized for the old icon size
    ComputeMetrics();
    const int x = m_monitor.left, y = m_monitor.bottom - m_height;
    m_full = {x, y, x + m_width, y + m_height};
    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, m_width, m_height, SWP_NOACTIVATE);
    if (m_state == State::Hidden) m_slide.Offset({0, static_cast<float>(m_height), 0});
    BuildDock();
    m_label = std::make_unique<halo::AppLabel>(m_comp, m_slide, m_gdev, m_scale.v);
    UpdateRegion();
    Log(L"dock rebuilt, icon " + std::to_wstring(static_cast<int>(m_metrics.restSize)) + L" px, window height " +
        std::to_wstring(m_height));
}

// A soft drop shadow for the dock pill: a brushless sprite that follows the pill's size and
// offset, its shadow shaped by a nine-grid rounded-rect mask. A sibling of the (clipped) pill.
wuc::SpriteVisual Halo::BuildPillShadow(wuc::ContainerVisual const& pill) {
    auto const& pal = halo::theme::P();
    if (!pal.shadow.a) return nullptr;
    const int ri = std::max(2, static_cast<int>(std::ceil(m_metrics.cornerRadius)));
    const int side = 2 * ri + 2;
    auto surface = MakeSurface({static_cast<float>(side), static_cast<float>(side)});
    auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT offset{};
    winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &offset));
    try {
        dc->Clear(D2D1::ColorF(0, 0.0f));
        winrt::com_ptr<ID2D1SolidColorBrush> white;
        winrt::check_hresult(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), white.put()));
        D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(offset.x), static_cast<float>(offset.y),
                                         static_cast<float>(offset.x + side), static_cast<float>(offset.y + side)),
                             static_cast<float>(ri), static_cast<float>(ri)};
        dc->FillRoundedRectangle(rr, white.get());
    } catch (...) {
        interop->EndDraw();
        throw;
    }
    winrt::check_hresult(interop->EndDraw());
    auto src = m_comp.CreateSurfaceBrush(surface);
    src.Stretch(wuc::CompositionStretch::Fill);
    auto grid = m_comp.CreateNineGridBrush();
    grid.Source(src);
    grid.SetInsets(static_cast<float>(ri));

    auto shadow = m_comp.CreateDropShadow();
    shadow.Mask(grid);
    shadow.Color(halo::theme::C({0xFF, pal.shadow.r, pal.shadow.g, pal.shadow.b}));
    shadow.Opacity(pal.shadow.a / 255.0f);
    shadow.BlurRadius(m_scale.f(pal.shadowBlur));
    shadow.Offset({0, m_scale.f(pal.shadowY), 0});
    auto sprite = m_comp.CreateSpriteVisual();
    sprite.Shadow(shadow);
    auto size = m_comp.CreateExpressionAnimation(L"pill.Size");
    size.SetReferenceParameter(L"pill", pill);
    sprite.StartAnimation(L"Size", size);
    auto off = m_comp.CreateExpressionAnimation(L"pill.Offset");
    off.SetReferenceParameter(L"pill", pill);
    sprite.StartAnimation(L"Offset", off);
    return sprite;
}

void Halo::BuildDock() {
    auto pill = BuildPill();
    m_cluster = std::make_unique<DockCluster>(m_comp, m_gdev, m_hwnd, m_metrics.pillHeight, m_scale.v);
    m_dock = std::make_unique<Dock>(m_comp, m_slide, pill, m_metrics, m_cluster.get());
    Step(L"pill shadow", [&] {
        auto shadow = BuildPillShadow(pill);
        if (shadow && pill.Parent()) pill.Parent().Children().InsertBelow(shadow, pill);
    });
    if (!m_tracker) m_tracker = std::make_unique<WindowTracker>(m_hwnd, kMsgAppsChanged);
    RefreshDock();
}

// The icon for a .lnk or exe, loaded once. Null if it has none.
wuc::CompositionBrush Halo::IconBrushFor(std::wstring const& path) {
    auto it = m_brushes.find(path);
    if (it != m_brushes.end()) return it->second;
    wuc::CompositionBrush brush{nullptr};
    try {
        IconImage icon;
        if (LoadJumboIcon(path, icon) && icon.width != 0)
            brush = MakeIconBrush(icon, m_metrics.restSize * m_metrics.maxScale);
        else
            Log(L"no icon for " + path);
    } catch (winrt::hresult_error const& e) {  // a lost device must not take the whole dock refresh with it
        Log(L"icon for " + path + L" failed " + Hex(e.code()));
    } catch (std::exception const&) {
        Log(L"icon for " + path + L" failed (std::exception)");
    }
    m_brushes.insert_or_assign(path, brush);
    return brush;
}

// The shell's plain application icon, for a pin or running app whose own icon can't be loaded, so it
// still has a slot to click (and to Remove from Dock) instead of vanishing. Loaded once.
wuc::CompositionBrush Halo::FallbackIconBrush() {
    static const std::wstring kKey = L"\x01generic";  // can't be a path
    auto it = m_brushes.find(kKey);
    if (it != m_brushes.end()) return it->second;
    wuc::CompositionBrush brush{nullptr};
    try {
        IconImage icon;
        if (LoadGenericIcon(icon) && icon.width != 0) brush = MakeIconBrush(icon, m_metrics.restSize * m_metrics.maxScale);
    } catch (winrt::hresult_error const& e) {
        Log(L"generic icon failed " + Hex(e.code()));
    } catch (std::exception const&) {
        Log(L"generic icon failed (std::exception)");
    }
    m_brushes.insert_or_assign(kKey, brush);
    return brush;
}

// Pinned apps first, then the running apps that aren't pinned.
void Halo::RefreshDock() {
    // A new window (Paint, say) can land above us in the topmost band; take the top back
    // before the slow part, loading its icon.
    SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (m_launcher) m_launcher->Raise();  // the panel stays above the dock
    if (m_menu) m_menu->Raise();
    auto running = m_tracker->Apps();
    std::vector<bool> pinned(running.size());
    std::vector<Dock::Item> items;
    m_items.clear();
    m_appNames.clear();
    // The pill can't grow past the screen: pinned apps take the room first, then running ones in the order they
    // appeared; the rest stay reachable from Alt+Tab and the Start panel's recents.
    const size_t cap = m_dock->MaxItems(static_cast<float>(m_width - m_scale.px(16)));
    for (auto const& pin : m_apps) {
        if (items.size() >= cap) break;
        auto brush = IconBrushFor(pin.linkPath);
        if (!brush) brush = FallbackIconBrush();  // uninstalled or unreadable: shown plain, removable from its menu
        if (!brush) continue;
        Item item{pin.linkPath, {}};
        if (!pin.targetPath.empty()) item.file = pin.targetPath;
        else if (!_wcsnicmp(pin.linkPath.c_str(), L"shell:", 6)) item.file.clear();
        else item.file = pin.linkPath;
        for (size_t i = 0; i < running.size(); i++) {
            auto const& r = running[i];
            if ((!pin.aumid.empty() && !_wcsicmp(pin.aumid.c_str(), r.aumid.c_str())) ||
                (!pin.targetPath.empty() && !_wcsicmp(pin.targetPath.c_str(), r.exe.c_str()))) {
                pinned[i] = true;
                if (item.key.empty()) item.key = r.key;
            }
        }
        items.push_back({L"p:" + pin.linkPath, brush, !item.key.empty(), false});
        m_items.push_back(item);
        m_appNames.push_back(pin.name);
    }
    for (size_t i = 0; i < running.size(); i++) {
        if (pinned[i] || running[i].exe.empty()) continue;
        if (items.size() >= cap) {
            Log(L"dock full (" + std::to_wstring(cap) + L" icons fit): " + running[i].key + L" and later apps not shown");
            break;
        }
        // Packaged apps' exes often carry no icon; their Start entry has the logo.
        wuc::CompositionBrush brush{nullptr};
        if (!running[i].aumid.empty()) brush = IconBrushFor(L"shell:AppsFolder\\" + running[i].aumid);
        if (!brush) brush = IconBrushFor(running[i].exe);
        if (!brush) brush = FallbackIconBrush();
        if (!brush) continue;
        items.push_back({L"u:" + running[i].key, brush, true, true});
        m_items.push_back({{}, running[i].key,
                           running[i].aumid.empty() ? running[i].exe : L"shell:AppsFolder\\" + running[i].aumid,
                           running[i].exe});

        std::wstring name;
        if (!running[i].aumid.empty()) {
            name = halo::AppNameResolver::Instance().Resolve(L"shell:AppsFolder\\" + running[i].aumid);
        }
        if (name.empty()) {
            name = halo::AppNameResolver::Instance().Resolve(running[i].key, running[i].exe);
        }
        m_appNames.push_back(name);
    }
    Log(std::to_wstring(running.size()) + L" running apps, " + std::to_wstring(items.size()) + L" icons");
    m_dock->SetItems(items);
    UpdateRegion();
    if (_wgetenv(L"HALO_TEST"))  // where a test can hover: the pointer x of each running app's icon
        for (size_t i = 0; i < m_items.size(); i++)
            if (!m_items[i].key.empty() || _wgetenv(L"HALO_TEST_MENU_DRYRUN"))
                Log(L"test icon " + std::to_wstring(i) + L" " + m_appNames[i] + L" x=" +
                    std::to_wstring(static_cast<int>(m_dock->RestIconCenter(i).first)) + L" windows=" +
                    std::to_wstring(m_tracker->WindowsOf(m_items[i].key).size()) + (m_items[i].key.empty() ? L" idle" : L" running") + (m_items[i].launch.empty() ? L" unpinned" : L" pinned"));
}

RECT Halo::GetPillRectScreen() const {
    if (!m_dock) return m_full;
    float half = m_dock->RestWidth() / 2.0f;
    int pillLeft = m_full.left + static_cast<int>(m_metrics.centerX - half);
    int pillRight = m_full.left + static_cast<int>(std::ceil(m_metrics.centerX + half));
    int pillTop = m_full.bottom - static_cast<int>(m_metrics.pillHeight + m_scale.px(6));
    int pillBottom = m_full.bottom;
    return RECT{pillLeft, pillTop, pillRight, pillBottom};
}

// The dock's sizes from the DPI scale and [taskbar] icon_size / magnification. icon_size 48 is the shipped
// 28 logical px icon; magnification 150 is the shipped 1.57x (hover magnification is off under reduce_motion).
void Halo::ComputeMetrics() {
    auto const& tb = halo::settings::Get().taskbar;
    m_metrics.restSize = m_scale.f(28 * tb.iconSize / 48.0f);
    m_metrics.gap = m_scale.f(10);
    m_metrics.maxScale = halo::settings::Get().reduceMotion ? 1.0f : 1.0f + (tb.magnification - 100) / 100.0f * 1.14f;
    m_metrics.radius = m_scale.f(100);
    m_metrics.padding = m_scale.f(10);
    m_metrics.pillHeight = m_metrics.restSize + m_scale.f(22);  // 50 at the shipped icon size
    m_metrics.cornerRadius = m_scale.f(20);
    // The window reaches the monitor's bottom edge so a hiding dock slides
    // fully off-screen; the dock itself sits a margin above it.
    int margin = m_scale.px(6);
    float peak = (m_metrics.pillHeight - m_metrics.restSize) / 2 + m_metrics.restSize * m_metrics.maxScale;
    // Above the peak: room for the hover name label (kLabelRoom).
    m_height = static_cast<int>(std::ceil(std::max(m_metrics.pillHeight, peak) + m_scale.f(kLabelRoom))) + margin;
    m_metrics.bottom = static_cast<float>(m_height - margin);
    m_metrics.centerX = m_width / 2.0f;
}

// Seats the dock on `mon`: the cursor reached its bottom edge, or (WM_DISPLAYCHANGE) the monitor it was on changed
// resolution or scale, or went away. The Dock keeps its own copy of the metrics (centre, bottom, icon size), so a
// different size or scale means a rebuild, not just moving the window.
void Halo::MigrateToMonitor(HMONITOR mon, bool reveal) {
    if (!mon) return;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return;

    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    float newScale = dpiX / 96.0f;
    bool dpiChanged = std::abs(m_scale.v - newScale) > 0.001f;
    if (mon == m_currentMon && !dpiChanged && EqualRect(&mi.rcMonitor, &m_monitor)) return;  // nothing moved

    const float iconPx = std::ceil(m_metrics.restSize * m_metrics.maxScale);
    m_currentMon = mon;
    m_monitor = mi.rcMonitor;
    m_width = m_monitor.right - m_monitor.left;
    m_scale.v = newScale;

    ComputeMetrics();

    int x = m_monitor.left, y = m_monitor.bottom - m_height;
    m_full = {x, y, x + m_width, y + m_height};

    SetWindowPos(m_hwnd, HWND_TOPMOST, x, y, m_width, m_height, SWP_NOACTIVATE);

    if (m_multimon) m_multimon->SetCurrentMonitor(mon);
    if (m_intellihide) m_intellihide->SetMonitor(mon, m_monitor);

    if (m_dock) RebuildDock(std::ceil(m_metrics.restSize * m_metrics.maxScale) == iconPx);  // also sets the label's scale, the input region and the intellihide rect
    else UpdateRegion();
    if (reveal) Reveal();
}

// Limits the window's input to the dock (and the bar of it a hidden dock
// leaves on the screen edge); clicks anywhere else on the bottom of the
// screen reach the windows underneath. Takes the width the dock can grow to.
void Halo::UpdateRegion() {
    if (!m_dock || !m_hwnd) return;
    bool hidden = m_state == State::Hidden;
    float half = (hidden ? m_dock->RestWidth() : m_dock->MaxWidth()) / 2;
    int left = std::max(0, static_cast<int>(m_metrics.centerX - half));
    int right = std::min(m_width, static_cast<int>(std::ceil(m_metrics.centerX + half)));
    SetWindowRgn(m_hwnd, CreateRectRgn(left, hidden ? m_height - kStripPx : 0, right, m_height), TRUE);
    if (m_intellihide) m_intellihide->SetDockRect(GetPillRectScreen());
    Log(L"input region x " + std::to_wstring(left) + L".." + std::to_wstring(right) + (hidden ? L" (strip)" : L""));
}

// Brings one window to the front, restoring it if minimized. Sent, not ShowWindow'd, so the
// restore runs in the app and the genie mod can animate it.
void Halo::ActivateWindow(HWND w) {
    HidePreviews();
    if (IsIconic(w)) SendMessageTimeoutW(w, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 200, nullptr);
    SetForegroundWindow(w);
}

// macOS: a running app comes to the front (its most recent window, restored if minimized) with a
// short bob; clicking it while it is already in front minimizes it; one that isn't running is launched.
void Halo::Activate(size_t item) {
    Item const& it = m_items[item];
    HidePreviews();
    KillTimer(m_hwnd, kPreviewTimer);
    if (HWND w = it.key.empty() ? nullptr : m_tracker->Front(it.key)) {
        HWND fg = GetForegroundWindow();
        if (_wgetenv(L"HALO_TEST"))
            Log(L"activate: key=" + it.key + L" front=" + std::to_wstring(reinterpret_cast<uintptr_t>(w)) + L" fg=" +
                std::to_wstring(reinterpret_cast<uintptr_t>(fg)) + L" fgKey=" + m_tracker->KeyOf(fg));
        if (fg && m_tracker->KeyOf(fg) == it.key) {
            for (HWND h : m_tracker->WindowsOf(it.key))
                if (!IsIconic(h)) SendMessageTimeoutW(h, WM_SYSCOMMAND, SC_MINIMIZE, 0, SMTO_ABORTIFHUNG, 200, nullptr);
            return;
        }
        m_dock->Bob(static_cast<int>(item));
        ActivateWindow(w);
    } else if (!it.launch.empty()) {
        // Bounce first: ShellExecute can block this thread for a while, the animation runs without it.
        if (m_launcher) m_launcher->Note(it.launch);
        if (it.key.empty() && m_cfg.taskbar.bounceOnLaunch) m_dock->Bounce(static_cast<int>(item));  // an app that is up (no window yet) doesn't bounce
        ShellExecuteW(nullptr, L"open", it.launch.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void Halo::HidePreviews(bool instant) {
    KillTimer(m_hwnd, kPreviewGrace);
    if (m_previews) m_previews->Hide(instant);
}

// Re-reads the pin list and rebuilds the dock from it.
void Halo::ReloadPins() {
    m_apps = halo::pins::Load();
    if (m_dock) RefreshDock();
}

// The right-click menu of dock icon `item`: its open windows, then Keep in / Remove from Dock, Show in Explorer, Quit.
void Halo::OpenMenuFor(int item) {
    if (!m_menu || !m_dock || item < 0 || item >= static_cast<int>(m_items.size())) return;
    Item const it = m_items[item];  // a copy: the menu outlives a RefreshDock
    const bool dry = _wgetenv(L"HALO_TEST_MENU_DRYRUN") != nullptr;
    const bool pinned = !it.launch.empty();
    const bool running = !it.key.empty();

    HidePreviews(true);
    KillTimer(m_hwnd, kPreviewTimer);
    if (m_label) m_label->Hide();
    m_dock->SetPressed(-1);

    std::vector<halo::MenuEntry> entries;
    if (running) {
        size_t more = 0;
        for (HWND w : m_tracker->WindowsOf(it.key)) {
            if (!IsWindow(w)) continue;
            if (entries.size() >= kMenuWindows) { more++; continue; }  // 30 windows of one app would run the menu off the screen
            wchar_t t[256] = {};
            GetWindowTextW(w, t, ARRAYSIZE(t));
            entries.push_back({t[0] ? t : L"Untitled", false, [this, w] { ActivateWindow(w); }});
        }
        if (more) entries.push_back({std::to_wstring(more) + (more == 1 ? L" more window" : L" more windows"), false, nullptr});
        if (!entries.empty()) entries.push_back({L"", true, nullptr});
    }
    if (pinned) {
        entries.push_back({L"Remove from Dock", false, [this, it, dry] {
            if (dry) { Log(L"menu: DRY RUN, would unpin " + it.launch); return; }
            if (halo::pins::Unpin(it.launch)) ReloadPins();
        }});
    } else if (!it.pinPath.empty()) {
        entries.push_back({L"Keep in Dock", false, [this, it] {
            if (halo::pins::Pin(it.pinPath)) ReloadPins();
        }});
    }
    if (!it.file.empty()) {
        entries.push_back({L"Show in Explorer", false, [it, dry] {
            if (dry) { Log(L"menu: DRY RUN, would show " + it.file + L" in Explorer"); return; }
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(SHParseDisplayName(it.file.c_str(), nullptr, &pidl, 0, nullptr)) && pidl) {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                CoTaskMemFree(pidl);
            } else {
                Log(L"menu: Show in Explorer failed for " + it.file);
            }
        }});
    }
    if (running) {
        if (!entries.empty()) entries.push_back({L"", true, nullptr});
        entries.push_back({L"Quit", false, [this, it, dry] {
            auto wins = m_tracker->WindowsOf(it.key);
            if (dry) { Log(L"menu: DRY RUN, would close " + std::to_wstring(wins.size()) + L" windows of " + it.key); return; }
            for (HWND w : wins) PostMessageW(w, WM_CLOSE, 0, 0);
        }});
    }
    if (entries.empty()) return;
    Log(L"menu: item " + std::to_wstring(item) + L" '" + (item < static_cast<int>(m_appNames.size()) ? m_appNames[item] : L"") +
        L"' pinned=" + (pinned ? L"1" : L"0") + L" running=" + (running ? L"1" : L"0"));
    auto [cx, topY] = m_dock->RestIconCenter(item);
    m_menu->Show(std::move(entries), m_full.left + static_cast<int>(cx), GetPillRectScreen().top, m_monitor, m_scale.v);
}

// (Re)lists the windows of the app the strip shows; follows its icon, or drops the strip if the app is gone.
void Halo::RefreshPreviews() {
    if (!m_previews || !m_previews->IsVisible() || !m_tracker || !m_dock) return;
    auto wins = m_tracker->WindowsOf(m_previewKey);
    for (size_t i = 0; i < m_items.size(); i++) {
        if (m_items[i].key != m_previewKey || wins.empty()) continue;
        auto [cx, topY] = m_dock->MagnifiedIconCenterAndTop(i);
        m_previews->Show(wins, m_full.left + static_cast<int>(cx), GetPillRectScreen().top, m_monitor, m_scale.v);
        return;
    }
    HidePreviews();
}

void Halo::ShowPreviewsFor(int item) {
    if (!m_previews || item < 0 || item >= static_cast<int>(m_items.size()) || m_items[item].key.empty()) return;
    m_previewKey = m_items[item].key;
    if (m_label) m_label->Hide();
    auto wins = m_tracker->WindowsOf(m_previewKey);
    auto [cx, topY] = m_dock->MagnifiedIconCenterAndTop(item);
    m_previews->Show(wins, m_full.left + static_cast<int>(cx), GetPillRectScreen().top, m_monitor, m_scale.v);
}

void Halo::PointerMove(float x, float y) {
    if (m_autoHide) Reveal();
    if (m_dock) m_dock->OnPointerMove(x);
    if (m_menu && m_menu->IsOpen()) return;  // no name label or preview strip over an open menu
    if (m_dock && m_previews) {
        int hit = m_dock->HitTest(x, y);
        bool running = hit >= 0 && hit < static_cast<int>(m_items.size()) && !m_items[hit].key.empty();
        if (hit != m_hoverItem) {
            m_hoverItem = hit;
            KillTimer(m_hwnd, kPreviewTimer);
            if (running && m_previews->IsVisible()) ShowPreviewsFor(hit);                    // follow to the next app
            else if (running) SetTimer(m_hwnd, kPreviewTimer, kPreviewDelayMs, nullptr);     // rest first
            else if (m_previews->IsVisible()) SetTimer(m_hwnd, kPreviewGrace, kPreviewGraceMs, nullptr);
        }
        if (running) KillTimer(m_hwnd, kPreviewGrace);
    }
    if (m_dock && m_label) {
        int hit = m_dock->HitTest(x, y);
        if (m_previews && m_previews->IsVisible()) {
            m_label->Hide();  // the strip names the windows
        } else if (hit >= 0 && hit < static_cast<int>(m_appNames.size()) && !m_appNames[hit].empty()) {
            auto [cx, topY] = m_dock->MagnifiedIconCenterAndTop(hit);
            m_label->Show(m_appNames[hit], cx, topY);
        } else {
            m_label->Hide();
        }
    }
}

void Halo::PointerLeave() {
    if (m_label) m_label->Hide();
    if (m_dock) m_dock->OnPointerLeave();
    m_hoverItem = -1;
    KillTimer(m_hwnd, kPreviewTimer);
    if (m_previews && m_previews->IsVisible()) {  // time to move onto the strip; the dock stays up meanwhile
        SetTimer(m_hwnd, kPreviewGrace, kPreviewGraceMs, nullptr);
        return;
    }
    if (m_autoHide && m_state == State::Shown) {
        if (!m_intellihide || m_intellihide->HasOverlap() || m_intellihide->IsFullscreen()) {
            SetTimer(m_hwnd, kHideTimer, std::max<UINT>(m_cfg.taskbar.hideDelay, USER_TIMER_MINIMUM), nullptr);
        }
    }
}

// The Windows key and the Start logo: the Launcher panel (search, recent apps and files) above a
// revealed dock. Not the real Start, which brings the hidden taskbar back with it. A second tap closes it.
void Halo::OpenLauncher() {
    Reveal();
    if (m_launcher) {
        if (m_launcher->IsOpen()) m_launcher->Hide();
        else m_launcher->Show(LauncherAnchorNow());
    }
}

// Above the dock's pill, ten logical px clear of it, centered on it.
halo::LauncherAnchor Halo::LauncherAnchorNow() const {
    RECT pill = GetPillRectScreen();
    return {(pill.left + pill.right) / 2, pill.top - m_scale.px(10), m_dock ? m_dock->RestWidth() : 0.0f, m_scale.v, m_monitor};
}

// How the launcher names an app: its pin if it has one, else its Start entry, else its exe.
std::wstring Halo::LaunchTarget(RunningApp const& app) const {
    for (auto const& pin : m_apps)
        if ((!pin.aumid.empty() && !_wcsicmp(pin.aumid.c_str(), app.aumid.c_str())) ||
            (!pin.targetPath.empty() && !_wcsicmp(pin.targetPath.c_str(), app.exe.c_str())))
            return pin.linkPath;
    return app.aumid.empty() ? app.exe : L"shell:AppsFolder\\" + app.aumid;
}

// A recent app clicked in the panel: like a dock icon, it comes to the front if it is running, else starts.
void Halo::LaunchRecent(std::wstring const& target) {
    for (auto const& app : m_tracker->Apps()) {
        if (_wcsicmp(LaunchTarget(app).c_str(), target.c_str())) continue;
        if (HWND w = m_tracker->Front(app.key)) {
            if (IsIconic(w)) SendMessageTimeoutW(w, WM_SYSCOMMAND, SC_RESTORE, 0, SMTO_ABORTIFHUNG, 200, nullptr);
            SetForegroundWindow(w);
            return;
        }
    }
    ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// The dock slides on m_slide (its Offset.Y is m_height while hidden, 0 at rest). [taskbar] effect picks the
// reveal and hide: rise / bounce slide it as before (bounce on a looser spring), swipe moves it sideways in
// from the right edge, fade and swoop (a 92 % scale from the bottom centre, with a short rise) bring it to
// rest and fade it; none cuts. speed divides the durations and the spring period, motion scales the travel
// and the overshoot. All on the compositor.
namespace {
using halo::settings::Effect;
std::chrono::milliseconds Scaled(float ms, float speed) {
    return std::chrono::milliseconds(std::max(1, static_cast<int>(std::lround(ms / speed))));
}
}  // namespace

// Takes input over the whole dock at once and brings it up from wherever it is.
void Halo::Reveal() {
    KillTimer(m_hwnd, kHideTimer);
    if (m_state == State::Shown) return;
    bool wasHidden = m_state == State::Hidden;
    m_state = State::Shown;
    if (wasHidden) UpdateRegion();
    ++m_hideGen;
    auto const& tb = halo::settings::Get().taskbar;
    const float speed = halo::settings::Speed(tb), amt = halo::settings::Amount(tb);
    const Effect eff = tb.effect;
    for (auto prop : {L"Offset", L"Offset.Y", L"Offset.X", L"Opacity", L"Scale"}) m_slide.StopAnimation(prop);
    if (eff == Effect::None) {
        m_slide.Offset({0, 0, 0});
        m_slide.Opacity(1);
        m_slide.Scale({1, 1, 1});
        return;
    }
    const bool slides = eff == Effect::Rise || eff == Effect::Bounce;
    const float travel = kDockTravel * m_scale.v * amt;
    float2 start{0, 0};  // where it comes from, relative to rest
    if (slides) start.y = m_height * std::min(amt, 1.0f);
    else if (eff == Effect::Swipe) start.x = kDockTravel * 2 * m_scale.v * amt;
    else if (eff == Effect::Swoop) start.y = travel;
    const bool fade = !slides || amt < 1.0f;
    if (wasHidden) {
        m_slide.Offset({start.x, start.y, 0});
        m_slide.Opacity(fade ? 0.0f : 1.0f);
        if (eff == Effect::Swoop) {
            m_slide.CenterPoint({m_width / 2.0f, static_cast<float>(m_height), 0});
            m_slide.Scale({0.92f, 0.92f, 1});
        }
    }
    const float damping = eff == Effect::Bounce ? 1.0f - 0.5f * std::min(amt, 1.4f) : kSpringDamping;
    auto spring = [&](wchar_t const* prop) {
        auto a = m_comp.CreateSpringScalarAnimation();
        a.FinalValue(0.0f);
        a.DampingRatio(damping);
        a.Period(Scaled(kSpringPeriodMs, speed));
        m_slide.StartAnimation(prop, a);
    };
    if (start.y != 0 || wasHidden) spring(L"Offset.Y");
    if (eff == Effect::Swipe) spring(L"Offset.X");
    if (eff == Effect::Swoop) {
        auto grow = m_comp.CreateVector3KeyFrameAnimation();
        grow.InsertKeyFrame(1.0f, {1, 1, 1}, m_comp.CreateCubicBezierEasingFunction({0.0f, 0.0f}, {0.2f, 1.0f}));
        grow.Duration(Scaled(kDockFadeMs, speed));
        m_slide.StartAnimation(L"Scale", grow);
    }
    if (fade) {
        auto in = m_comp.CreateScalarKeyFrameAnimation();
        in.InsertKeyFrame(1.0f, 1.0f, m_comp.CreateCubicBezierEasingFunction({0.0f, 0.0f}, {0.2f, 1.0f}));
        in.Duration(Scaled(kDockFadeMs, speed));
        m_slide.StartAnimation(L"Opacity", in);
    }
}

void Halo::StartHide() {
    if (m_state != State::Shown) return;
    if (!m_cfg.taskbar.autohide) return;             // [taskbar] autohide off: the dock stays
    if (m_launcher && m_launcher->IsOpen()) return;  // the panel is up; its close brings the hide back
    if (m_menu && m_menu->IsOpen()) return;          // likewise the right-click menu
    m_state = State::Hiding;
    HidePreviews(true);
    if (m_label) m_label->Hide();
    if (m_dock) m_dock->OnPointerLeave();  // magnification relaxes as the dock drops
    unsigned gen = ++m_hideGen;
    auto const& tb = halo::settings::Get().taskbar;
    const float speed = halo::settings::Speed(tb), amt = halo::settings::Amount(tb);
    const Effect eff = tb.effect;
    // Off-screen, and back at the neutral pose the next reveal starts from.
    auto finish = [this, gen] {
        if (gen != m_hideGen || m_state != State::Hiding) return;  // a reveal took over
        m_state = State::Hidden;
        m_slide.Offset({0, static_cast<float>(m_height), 0});
        m_slide.Opacity(1);
        m_slide.Scale({1, 1, 1});
        UpdateRegion();
    };
    if (eff == Effect::None) { finish(); return; }
    for (auto prop : {L"Offset", L"Offset.Y", L"Offset.X", L"Opacity", L"Scale"}) m_slide.StopAnimation(prop);
    auto batch = m_comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    auto down = m_comp.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.9f, 0.5f});
    auto dur = Scaled(kHideDurationMs, speed);
    auto to = [&](wchar_t const* prop, float v) {
        auto a = m_comp.CreateScalarKeyFrameAnimation();
        a.InsertKeyFrame(1.0f, v, down);
        a.Duration(dur);
        m_slide.StartAnimation(prop, a);
    };
    if (eff == Effect::Rise || eff == Effect::Bounce) {
        to(L"Offset.Y", static_cast<float>(m_height));
    } else {
        to(L"Opacity", 0.0f);
        if (eff == Effect::Swipe) to(L"Offset.X", kDockTravel * 2 * m_scale.v * amt);
        if (eff == Effect::Swoop) {
            to(L"Offset.Y", kDockTravel * m_scale.v * amt);
            m_slide.CenterPoint({m_width / 2.0f, static_cast<float>(m_height), 0});
            auto shrink = m_comp.CreateVector3KeyFrameAnimation();
            shrink.InsertKeyFrame(1.0f, {0.92f, 0.92f, 1}, down);
            shrink.Duration(dur);
            m_slide.StartAnimation(L"Scale", shrink);
        }
    }
    batch.End();
    batch.Completed([finish](auto&&, auto&&) { finish(); });
}

// Cross-process protocol with the Halo Genie Windhawk mod (mods/halo-genie.wh.cpp),
// which asks where a window's dock icon is so minimize/restore can swoop into it.
//   message: registered "Halo.DockIconForWindow", sent to the HaloDockWindow
//   wParam:  the HWND being animated; lParam: 1 minimize, 2 restore, 3 query only
//   result:  0 = no dock icon for that window, else the icon's rest position
//            (dock shown, not slid off) in physical screen px, 48 bits:
//              bits  0..15 icon size (square, unsigned)
//              bits 16..31 centre x (signed 16-bit; negative on monitors left of the primary)
//              bits 32..47 centre y (signed 16-bit)
//   On 1 and 2 the dock is also revealed and held for kGenieHoldMs so the swoop lands on a visible icon.
LRESULT Halo::DockIconForWindow(HWND win, LPARAM op) {
    if (!m_dock || !m_tracker) return 0;
    std::wstring key = m_tracker->KeyOf(win);
    if (key.empty()) return 0;
    for (size_t i = 0; i < m_items.size(); i++) {
        if (m_items[i].key != key) continue;
        if (op != 3) {
            Reveal();
            if (!m_trackingMouse) SetTimer(m_hwnd, kHideTimer, kGenieHoldMs, nullptr);
        }
        auto [x, y] = m_dock->RestIconCenter(i);
        auto pack = [](float v) { return static_cast<ULONGLONG>(static_cast<uint16_t>(static_cast<int16_t>(std::lround(v)))); };
        ULONGLONG size = static_cast<ULONGLONG>(std::lround(m_metrics.restSize)) & 0xFFFF;
        return static_cast<LRESULT>(size | pack(x + m_full.left) << 16 | pack(y + m_full.top) << 32);
    }
    return 0;
}

LRESULT CALLBACK Halo::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        auto* self = static_cast<Halo*>(cs->lpCreateParams);
        self->m_hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Halo*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    // A throw out of a handler (a failed icon surface, a rejected expression, a dead device) would unwind
    // through user32 and take the whole shell with it. Log it and carry on: the next event retries.
    try {
        return self->OnMessage(m, w, l);
    } catch (winrt::hresult_error const& e) {
        Log(L"message " + std::to_wstring(m) + L" threw " + Hex(e.code()) + L" " + e.message().c_str());
    } catch (std::exception const&) {
        Log(L"message " + std::to_wstring(m) + L" threw std::exception");
    }
    return DefWindowProcW(h, m, w, l);
}

// withNotifs: the notification column comes too, stacked above it (Win+A / Win+N, touch); the bottom-right
// corner brings the Control Center alone.
void Halo::OpenPanel(bool byPointer, bool withNotifs) {
    if (!m_cc) return;
    if (!m_cc->IsOpen()) {
        m_cc->Open(byPointer);
        if (!m_cc->IsOpen()) return;
        Log(L"panel: opened");
    } else if (!withNotifs || (m_notif && m_notif->IsOpen())) {
        return;
    }
    if (m_notif && withNotifs) {
        m_notif->SetAnchor(m_cc->PanelRectScreen());
        SetTimer(m_hwnd, kPanelStagger, kPanelStaggerMs, nullptr);  // follows ~40 ms later with the same motion
    }
}

void Halo::ClosePanel() {
    KillTimer(m_hwnd, kPanelStagger);
    if (m_cc) m_cc->Close();   // its onHidden closes the notification column too
    if (m_notif) m_notif->Close();
}

LRESULT Halo::OnMessage(UINT m, WPARAM w, LPARAM l) {
    const float mx = static_cast<float>(static_cast<short>(LOWORD(l)));
    const float my = static_cast<float>(static_cast<short>(HIWORD(l)));
    if (m == m_genieMsg && m) return DockIconForWindow(reinterpret_cast<HWND>(w), l);
    if (m == halo::winkey::Message()) { OpenLauncher(); return 0; }  // Win tapped alone
    if (m == halo::winkey::EscapeMessage()) {                        // Esc, while the panel or a menu is up
        if (m_menu && m_menu->IsOpen()) m_menu->Close();
        else if (m_launcher) m_launcher->Hide();
        return 0;
    }
    if (m == halo::winkey::ControlCenterMessage()) {  // Win+A (wParam 0) / Win+N (1): both toggle the one panel
        TogglePanel();
        return 0;
    }
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!m_trackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
                m_trackingMouse = TrackMouseEvent(&tme);
            }
            PointerMove(mx, my);
            return 0;
        case WM_MOUSELEAVE:
            m_trackingMouse = false;
            PointerLeave();
            return 0;
        case kMsgTestMenu:
            if (m_menu && _wgetenv(L"HALO_TEST")) {
                if (l == 0) { Reveal(); OpenMenuFor(static_cast<int>(w)); }
                else if (l == 1) m_menu->Close();
                else if (l >= 100 && l < 200) m_menu->TestHover(static_cast<int>(l) - 100);
                else if (l >= 200) m_menu->TestActivate(static_cast<int>(l) - 200);
            }
            return 0;
        case kMsgTestBounce: if (m_dock && _wgetenv(L"HALO_TEST")) m_dock->Bounce(static_cast<int>(w)); return 0;
        case kMsgTestMove: Reveal(); PointerMove(static_cast<float>(w), m_metrics.bottom - m_metrics.pillHeight / 2.0f); return 0;
        case kMsgTestLeave: PointerLeave(); return 0;
        case kMsgTestLauncher:
            if (m_launcher && !m_autoHide) {
                if (w == 0) m_launcher->Show(LauncherAnchorNow());
                else if (w == 1) m_launcher->Hide();
                else m_launcher->TestCommand(w);
            }
            return 0;
        case kMsgTestControlCenter: case kMsgTestNotifCenter: if (_wgetenv(L"HALO_TEST")) TogglePanel(); return 0;
        case kMsgIntellihideChanged:
            if (w || l) {
                if (!m_trackingMouse) StartHide();
            } else {
                Reveal();
            }
            return 0;
        case kMsgMonitorSwitch:
            MigrateToMonitor(reinterpret_cast<HMONITOR>(w));
            return 0;
        case WM_DISPLAYCHANGE:
            if (m_multimon) m_multimon->OnDisplayChange();
            halo::edge::OnDisplayChange();
            {  // the dock's monitor may be gone (unplugged, laptop docked / undocked) or have a new size or scale
                MONITORINFO mi{sizeof(mi)};
                HMONITOR mon = m_currentMon;
                if (!mon || !GetMonitorInfoW(mon, &mi)) mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
                MigrateToMonitor(mon, false);  // no reveal: nothing summoned it, and nothing would hide it again
            }
            return 0;
        case kMsgEdge:
            Log(w == halo::edge::kBottom ? L"edge: bottom right" : w == halo::edge::kTop ? L"edge: top" : L"edge: right");
            if (w == halo::edge::kBottom) {  // a fullscreen video's controls live there: not us. (Intellihide's
                // "fullscreen" also counts any maximised window, since the taskbar is hidden: ask the shell.)
                QUERY_USER_NOTIFICATION_STATE q{};
                if (SUCCEEDED(SHQueryUserNotificationState(&q)) &&
                    (q == QUNS_BUSY || q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE))
                    return 0;
            }
            OpenPanel(w == halo::edge::kBottom, w != halo::edge::kBottom);  // the corner: Quick Settings alone; touch: both, no auto-hide
            return 0;
        case WM_TIMER:
            if (w == kHideTimer) {
                KillTimer(m_hwnd, kHideTimer);
                // Only a window over the dock, or a fullscreen app, takes it away (PointerLeave arms the timer on
                // that condition too). The genie hold arms it regardless: with nothing in the way the dock stays up.
                if (!m_intellihide || m_intellihide->HasOverlap() || m_intellihide->IsFullscreen()) StartHide();
            } else if (w == kPanelStagger) {
                KillTimer(m_hwnd, kPanelStagger);
                if (m_cc && m_cc->IsOpen() && m_notif) {
                    m_notif->SetAnchor(m_cc->PanelRectScreen());
                    m_notif->Open();
                    Log(L"panel: notification column opened");
                }
            } else if (w == kPreviewTimer) {
                KillTimer(m_hwnd, kPreviewTimer);
                if (m_hoverItem >= 0 && m_state == State::Shown) ShowPreviewsFor(m_hoverItem);
            } else if (w == kPreviewGrace) {
                KillTimer(m_hwnd, kPreviewGrace);
                if (m_previews && !m_previews->ContainsCursor()) {
                    m_previews->Hide();
                    if (!m_trackingMouse) PointerLeave();  // now the dock may auto-hide
                }
            } else if (w == DockCluster::kClockTimer) {
                if (m_dock) m_dock->OnClockTimer();
            }
            return 0;
        case WM_TIMECHANGE:
            if (m_dock) m_dock->OnTimeChange();
            return 0;
        case kMsgGuardianGone:
            StartAdopter();
            return 0;
        case kMsgSettings:
            if (halo::settings::Reload()) ApplySettings();
            return 0;
        case WM_SETTINGCHANGE:
            if (l && !wcscmp(reinterpret_cast<LPCWSTR>(l), L"ImmersiveColorSet")) {  // the Windows light / dark switch
                const bool was = halo::theme::Dark();
                halo::theme::Refresh();
                if (halo::theme::Dark() != was) ApplySettings(true);
            }
            if (m_dock) m_dock->OnSettingChange();
            return 0;
        case WM_POWERBROADCAST:
            if (m_dock) m_dock->OnPowerBroadcast(w, l);
            return 0;
        case DockCluster::kMsgNetworkChanged:
            if (m_dock) m_dock->OnNetworkChanged();
            return 0;
        case WM_LBUTTONDOWN:
            if (m_dock && m_state == State::Shown) {
                m_pressedStart = m_dock->HitStart(mx, my);
                m_pressed = m_dock->HitTest(mx, my);
                m_dock->SetPressed(m_pressed);
                if (m_pressed >= 0 && (GetKeyState(VK_LBUTTON) & 0x8000)) SetCapture(m_hwnd);  // the release may land outside
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (std::exchange(m_pressedStart, false) && m == WM_LBUTTONUP && m_dock && m_dock->HitStart(mx, my))
                OpenLauncher();
            if (m_pressed >= 0 && m_dock) {
                int pressed = std::exchange(m_pressed, -1);
                m_dock->SetPressed(-1);
                if (m == WM_LBUTTONUP) {
                    ReleaseCapture();
                    if (m_state == State::Shown && m_dock->HitTest(mx, my) == pressed &&
                        pressed < static_cast<int>(m_items.size()))
                        Activate(pressed);
                }
            }
            return 0;
        case WM_RBUTTONUP:
            if (m_dock && m_state == State::Shown) {
                int hit = m_dock->HitTest(mx, my);
                if (hit >= 0 && hit < static_cast<int>(m_items.size())) OpenMenuFor(hit);
            }
            return 0;
        case kMsgAppsChanged:
            if (m_dock) RefreshDock();
            RefreshPreviews();
            return 0;
        case WM_HOTKEY:
            if (w == kHotkeyQuit) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_CLOSE: m_menu.reset(); m_previews.reset(); m_cc.reset(); m_notif.reset(); KillTimer(m_hwnd, kHideTimer); DestroyWindow(m_hwnd); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(m_hwnd, m, w, l);
}

int Halo::Run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    halo::winkey::Remove();
    halo::edge::Stop();
    UnregisterHotKey(m_hwnd, kHotkeyQuit);
    return static_cast<int>(msg.wParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    DWORD guardianPid = 0;
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i + 1 < argc; ++i)
            if (_wcsicmp(argv[i], L"--guardian") == 0) guardianPid = static_cast<DWORD>(_wtoi(argv[i + 1]));
        LocalFree(argv);
    }
    int code;
    {
        Halo app;
        if (!app.Create(inst)) {
            // A window with no dock must not look like a running shell: the guardian hides the taskbar
            // once the dock window is visible, and a non-zero exit makes it restore and retry.
            Log(L"startup incomplete; exiting");
            return 2;
        }
        if (guardianPid) WatchGuardian(guardianPid, app.Hwnd());
        code = app.Run();
    }
    if (g_unguarded) halo::taskbar::RestoreAll();
    return code;
}
