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
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <memory>
#include <shellapi.h>
#include <string>
#include <vector>
#include "dock.h"
#include "effects.h"
#include "icons.h"

// Interop interfaces missing from the MinGW SDK headers (layouts per the Windows SDK).
struct ICompositorDesktopInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopWindowTarget(HWND hwnd, BOOL isTopmost, void** target) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnsureOnThread(DWORD threadId) = 0;
};
__CRT_UUID_DECL(ICompositorDesktopInterop, 0x29E691FA, 0x4567, 0x4DCA, 0xB3, 0x19, 0xD0, 0xF2, 0x07, 0xEB, 0x68, 0x07)
struct ICompositionDrawingSurfaceInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE BeginDraw(RECT const* updateRect, REFIID iid, void** updateObject, POINT* offset) = 0;
    virtual HRESULT STDMETHODCALLTYPE EndDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resize(SIZE size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Scroll(RECT const* scrollRect, RECT const* clipRect, int dx, int dy) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeDraw() = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendDraw() = 0;
};
__CRT_UUID_DECL(ICompositionDrawingSurfaceInterop, 0xFD04E6E3, 0xFE0C, 0x4C3C, 0xAB, 0x19, 0xA0, 0x76, 0x01, 0xA5, 0x76, 0xEE)
extern "C" HRESULT WINAPI D2D1CreateDevice(IDXGIDevice*, D2D1_CREATION_PROPERTIES const*, ID2D1Device**);

namespace wuc = winrt::Windows::UI::Composition;
using winrt::Windows::Foundation::Size;

namespace {

// Settings
constexpr bool kAutoHide = true;
constexpr UINT kHideDelayMs = 500;        // cursor must stay away this long before hiding
constexpr float kSpringDamping = 0.72f;   // < 1 overshoots slightly
constexpr int kSpringPeriodMs = 55;       // lower = snappier
constexpr int kHideDurationMs = 200;
constexpr int kStripPx = 2;               // hit strip left on the screen edge while hidden

constexpr int kHotkeyQuit = 1;
constexpr UINT_PTR kHideTimer = 1;
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

class Halo {
public:
    bool Create(HINSTANCE inst);
    int Run();

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT, WPARAM, LPARAM);
    void InitComposition();
    void InitGraphics();
    wuc::ContainerVisual BuildPill();
    void BuildDock();
    void Reveal();
    void StartHide();
    wuc::CompositionDrawingSurface MakeSurface(Size size);
    wuc::CompositionBrush MakeIconBrush(IconImage const& icon, float size);

    HWND m_hwnd{};
    Scale m_scale{1.0f};
    int m_width{}, m_height{};
    DockMetrics m_metrics;
    std::vector<PinnedApp> m_apps;
    std::unique_ptr<Dock> m_dock;
    bool m_trackingMouse = false;

    enum class State { Hidden, Shown, Hiding };
    State m_state = State::Shown;
    unsigned m_hideGen = 0;  // invalidates a hide batch that a reveal interrupted
    RECT m_full{}, m_strip{};
    wuc::ContainerVisual m_slide{nullptr};  // the dock's parent; its Offset.Y slides the dock off-screen

    winrt::Windows::System::DispatcherQueueController m_dq{nullptr};
    wuc::Compositor m_comp{nullptr};
    wuc::Desktop::DesktopWindowTarget m_target{nullptr};
    wuc::ContainerVisual m_root{nullptr};

    winrt::com_ptr<ID3D11Device> m_d3d;
    winrt::com_ptr<ID2D1Device> m_d2d;
    wuc::CompositionGraphicsDevice m_gdev{nullptr};
};

bool Halo::Create(HINSTANCE inst) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HaloDockWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&wc)) return false;

    HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpiX = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
    m_scale.v = dpiX / 96.0f;

    Step(L"pinned apps", [&] { m_apps = LoadPinnedApps(); });

    // Metrics in physical px. The window is as wide as the dock can grow and
    // tall enough for a fully magnified icon above the pill.
    m_metrics.restSize = m_scale.f(44);
    m_metrics.gap = m_scale.f(10);
    m_metrics.maxScale = 1.6f;
    m_metrics.radius = m_scale.f(130);
    m_metrics.padding = m_scale.f(12);
    m_metrics.pillHeight = m_scale.f(64);
    m_metrics.cornerRadius = m_scale.f(20);
    float pitch = m_metrics.restSize + m_metrics.gap;
    float n = static_cast<float>(std::max<size_t>(m_apps.size(), 1));
    float growth = 3.0f * pitch * (m_metrics.maxScale - 1);  // generous bound on how far the wave widens it
    m_width = static_cast<int>(std::ceil(n * pitch + 2 * m_metrics.padding + growth + m_scale.f(40)));
    // The window reaches the monitor's bottom edge so a hiding dock slides
    // fully off-screen; the dock itself sits a margin above it.
    int margin = m_scale.px(8);
    m_height = static_cast<int>(
        std::ceil(m_metrics.pillHeight + m_metrics.restSize * (m_metrics.maxScale - 1) + m_scale.f(16))) + margin;
    m_metrics.centerX = m_width / 2.0f;
    m_metrics.bottom = static_cast<float>(m_height - margin);

    int x = mi.rcMonitor.left + ((mi.rcMonitor.right - mi.rcMonitor.left) - m_width) / 2;
    int y = mi.rcMonitor.bottom - m_height;
    m_full = {x, y, x + m_width, y + m_height};
    m_strip = {x, mi.rcMonitor.bottom - kStripPx, x + m_width, mi.rcMonitor.bottom};
    RECT const& first = kAutoHide ? m_strip : m_full;
    if (kAutoHide) m_state = State::Hidden;

    m_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                             wc.lpszClassName, L"Halo", WS_POPUP, first.left, first.top, first.right - first.left,
                             first.bottom - first.top, nullptr, nullptr, inst, this);
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
    }
    if (!RegisterHotKey(m_hwnd, kHotkeyQuit, MOD_CONTROL | MOD_WIN | MOD_NOREPEAT, VK_ESCAPE))
        Log(L"RegisterHotKey Ctrl+Win+Esc failed " + std::to_wstring(GetLastError()));

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
    if (kAutoHide) m_slide.Offset({0, static_cast<float>(m_height), 0});
    m_root.Children().InsertAtTop(m_slide);
}

void Halo::InitGraphics() {
    winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, m_d3d.put(), nullptr, nullptr));
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

// The frosted glass: blurred host backdrop, a dark tint and a 1px top rim.
// Layers size relative to the container, which the dock resizes and clips.
wuc::ContainerVisual Halo::BuildPill() {
    auto pill = m_comp.CreateContainerVisual();

    auto glass = m_comp.CreateSpriteVisual();
    glass.RelativeSizeAdjustment({1, 1});
    Step(L"blur effect brush", [&] {
        auto backdrop = m_comp.CreateHostBackdropBrush();
        auto blur = winrt::make<halo::GaussianBlurEffect>(wuc::CompositionEffectSourceParameter(L"backdrop"), 30.0f);
        auto factory = m_comp.CreateEffectFactory(blur.as<winrt::Windows::Graphics::Effects::IGraphicsEffect>());
        auto brush = factory.CreateBrush();
        brush.SetSourceParameter(L"backdrop", backdrop);
        glass.Brush(brush);
    });
    pill.Children().InsertAtTop(glass);

    auto tint = m_comp.CreateSpriteVisual();
    tint.RelativeSizeAdjustment({1, 1});
    tint.Brush(m_comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0x80, 0x16, 0x16, 0x1A)));
    pill.Children().InsertAtTop(tint);

    auto rim = m_comp.CreateSpriteVisual();
    rim.RelativeSizeAdjustment({1, 0});
    rim.Size({0, std::max(1.0f, m_scale.f(1))});
    rim.Brush(m_comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0x4D, 0xFF, 0xFF, 0xFF)));
    pill.Children().InsertAtTop(rim);
    return pill;  // the dock places it in the tree
}

void Halo::BuildDock() {
    auto pill = BuildPill();
    m_dock = std::make_unique<Dock>(m_comp, m_slide, pill, m_metrics);

    std::vector<wuc::CompositionBrush> brushes;
    std::vector<PinnedApp> shown;
    float iconPx = m_metrics.restSize * m_metrics.maxScale;
    for (auto const& app : m_apps) {
        IconImage icon;
        if (!LoadJumboIcon(app.linkPath, icon) || icon.width == 0) {
            Log(L"no icon for " + app.name);
            continue;
        }
        brushes.push_back(MakeIconBrush(icon, iconPx));
        shown.push_back(app);
    }
    m_apps = std::move(shown);
    m_dock->SetIcons(brushes);
}

// Shows the full window at once and springs the dock up from wherever it is.
void Halo::Reveal() {
    KillTimer(m_hwnd, kHideTimer);
    if (m_state == State::Shown) return;
    if (m_state == State::Hidden)
        SetWindowPos(m_hwnd, HWND_TOPMOST, m_full.left, m_full.top, m_full.right - m_full.left,
                     m_full.bottom - m_full.top, SWP_NOACTIVATE);
    m_state = State::Shown;
    ++m_hideGen;
    auto spring = m_comp.CreateSpringScalarAnimation();
    spring.FinalValue(0.0f);
    spring.DampingRatio(kSpringDamping);
    spring.Period(std::chrono::milliseconds(kSpringPeriodMs));
    m_slide.StartAnimation(L"Offset.Y", spring);
}

void Halo::StartHide() {
    if (m_state != State::Shown) return;
    m_state = State::Hiding;
    unsigned gen = ++m_hideGen;
    auto batch = m_comp.CreateScopedBatch(wuc::CompositionBatchTypes::Animation);
    auto slide = m_comp.CreateScalarKeyFrameAnimation();
    slide.InsertKeyFrame(1.0f, static_cast<float>(m_height),
                         m_comp.CreateCubicBezierEasingFunction({0.5f, 0.0f}, {0.9f, 0.5f}));
    slide.Duration(std::chrono::milliseconds(kHideDurationMs));
    m_slide.StartAnimation(L"Offset.Y", slide);
    batch.End();
    batch.Completed([this, gen](auto&&, auto&&) {
        if (gen != m_hideGen || m_state != State::Hiding) return;  // a reveal took over
        m_state = State::Hidden;
        SetWindowPos(m_hwnd, HWND_TOPMOST, m_strip.left, m_strip.top, m_strip.right - m_strip.left,
                     m_strip.bottom - m_strip.top, SWP_NOACTIVATE);
    });
}

LRESULT CALLBACK Halo::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        auto* self = static_cast<Halo*>(cs->lpCreateParams);
        self->m_hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto* self = reinterpret_cast<Halo*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->OnMessage(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT Halo::OnMessage(UINT m, WPARAM w, LPARAM l) {
    const float mx = static_cast<float>(static_cast<short>(LOWORD(l)));
    const float my = static_cast<float>(static_cast<short>(HIWORD(l)));
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!m_trackingMouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, m_hwnd, 0};
                m_trackingMouse = TrackMouseEvent(&tme);
            }
            if (kAutoHide) Reveal();
            if (m_dock) m_dock->OnPointerMove(mx);
            return 0;
        case WM_MOUSELEAVE:
            m_trackingMouse = false;
            if (m_dock) m_dock->OnPointerLeave();
            if (kAutoHide && m_state == State::Shown) SetTimer(m_hwnd, kHideTimer, kHideDelayMs, nullptr);
            return 0;
        case WM_TIMER:
            if (w == kHideTimer) {
                KillTimer(m_hwnd, kHideTimer);
                StartHide();
            }
            return 0;
        case WM_LBUTTONUP:
            if (m_dock && m_state == State::Shown) {
                int i = m_dock->HitTest(mx, my);
                if (i >= 0 && i < static_cast<int>(m_apps.size())) {
                    ShellExecuteW(nullptr, L"open", m_apps[i].linkPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
            }
            return 0;
        case WM_HOTKEY:
            if (w == kHotkeyQuit) PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
            return 0;
        case WM_CLOSE: KillTimer(m_hwnd, kHideTimer); DestroyWindow(m_hwnd); return 0;
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
    UnregisterHotKey(m_hwnd, kHotkeyQuit);
    return static_cast<int>(msg.wParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    winrt::init_apartment(winrt::apartment_type::single_threaded);
    Halo app;
    if (!app.Create(inst)) Log(L"startup incomplete");
    return app.Run();
}
