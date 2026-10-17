#include "control_center.h"

#include <d2d1_1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellscalingapi.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#include "effects.h"
#include "services.h"

// Interop interfaces missing from the MinGW SDK headers (same layouts as halo.cpp).
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

namespace wuc = winrt::Windows::UI::Composition;
namespace wfn = winrt::Windows::Foundation::Numerics;
using namespace std::chrono_literals;
using D2D1::ColorF;

namespace halo {
namespace {

// Grid, in logical px (all drawing is scaled by the monitor DPI): 12 outer padding and gap between
// tiles, 10 between small tiles, 14/12 tile radii, 22 panel radius.
constexpr float kW = 348, kPad = 12, kGap = 12, kRadius = 22, kTileR = 14, kSmallR = 12, kCol = 156;
constexpr float kMaxH = 900, kMargin = 8;
constexpr int kMaxOutputs = 6;

constexpr UINT kMsgAudio = WM_APP + 1, kMsgMic = WM_APP + 2, kMsgRadios = WM_APP + 3, kMsgBright = WM_APP + 4,
               kMsgMedia = WM_APP + 5, kMsgWifi = WM_APP + 6, kMsgBt = WM_APP + 7;
constexpr UINT_PTR kTimerClose = 1, kTimerHot = 1, kTimerChord = 2;
constexpr UINT kCloseMs = 130, kHotDwellMs = 180;
constexpr wchar_t kPanelClass[] = L"HaloControlCenter", kHotClass[] = L"HaloControlCenterHotZone";

enum class Id {
    None, Header, Power, ConnTile, Wifi, WifiChev, Bluetooth, BtChev, Airplane, Hotspot, Dnd, Dark, Night, Mic, Energy,
    Snip, Cast, Project, DisplayTile, Bright, SoundTile, Vol, OutRow, OutDev, ListTile, WifiNet, BtDev, MediaTile, Prev,
    PlayPause, Next
};
bool Passive(Id id) {
    return id == Id::Header || id == Id::ConnTile || id == Id::DisplayTile || id == Id::SoundTile || id == Id::ListTile ||
           id == Id::MediaTile;
}
enum class Expand { None, Wifi, Bluetooth };

struct Item {
    Id id = Id::None;
    D2D1_RECT_F r{};
    int arg = 0;
    float px = 0, py = 0;  // where a toggle's icon circle is (the pulse ring starts there)
};
bool Same(Item const& a, Item const& b) { return a.id == b.id && a.arg == b.arg; }

winrt::Windows::Foundation::IReference<wfn::float3> Ref3(float v) {
    return winrt::Windows::Foundation::IReference<wfn::float3>(wfn::float3{v, v, 1.0f});
}
bool In(D2D1_RECT_F const& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

const D2D1_COLOR_F kText = ColorF(1, 1, 1, 0.95f), kDim = ColorF(1, 1, 1, 0.58f), kOff = ColorF(1, 1, 1, 0.17f),
                   kAccent = ColorF(0x0A84FF), kFill = ColorF(1, 1, 1, 0.94f), kOnFill = ColorF(0x1C1C1E);

}  // namespace

struct ControlCenter::Impl {
    ControlCenter* api;
    HINSTANCE inst;
    HWND owner;
    wuc::Compositor comp;
    wuc::CompositionGraphicsDevice gdev;

    // ---- window + composition (exist only while open) ----
    enum class St { Closed, Open, Closing } st = St::Closed;
    HWND hwnd{}, hot{};
    float s = 1;
    RECT mon{}, rect{};
    float height = 0;  // logical
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr}, pill{nullptr}, fx{nullptr};
    wuc::SpriteVisual content{nullptr};
    wuc::CompositionDrawingSurface surf{nullptr};
    wuc::CompositionRoundedRectangleGeometry clip{nullptr}, rim{nullptr};

    winrt::com_ptr<IDWriteFactory> dw;
    winrt::com_ptr<IDWriteTextFormat> fTime, fDate, fTitle, fSub, fSubR, fBatt, fIcon, fIconS, fPill, fCap;

    // ---- state ----
    svc::AudioEndpoint speaker, mic;
    svc::Radios radios;
    svc::Media media;
    std::vector<svc::Device> outs;
    bool outsOpen = false;
    Expand expand = Expand::None;
    float volume = 0;
    bool muted = false, micMuted = false, dnd = false, wifiOn = false, btOn = false, dark = false, energy = false;
    bool hotspotAvail = false, hotspotOn = false, savedWifi = true, savedBt = true;
    int bright = -1;
    svc::Battery battery;
    svc::PowerMode mode = svc::PowerMode::Unknown;
    std::wstring ssid;
    std::vector<Item> items;
    Item hover, pressed;
    bool tracking = false;
    Id drag = Id::None;
    ULONGLONG lastBrightSet = 0;
    WORD chordKey = 0;
    bool chordShift = false;

    // ---- hot zone ----
    bool hotTracking = false;
    POINT touchStart{-1, -1};

    Impl(ControlCenter* a, HINSTANCE i, HWND o, wuc::Compositor c, wuc::CompositionGraphicsDevice g)
        : api(a), inst(i), owner(o), comp(std::move(c)), gdev(std::move(g)) {}

    void RegisterClasses();
    void PlaceHotZone();
    bool FlickedIn();
    void Open();
    void Close();
    void Teardown();
    void Focus();
    void BuildFonts();
    void SyncAll();
    void SyncSpeaker();
    void SyncMic();
    void SyncRadios();
    void Layout();
    void ApplySize();
    void Render();
    void BuildVisuals();
    wuc::CompositionBrush NoiseBrush(int w, int h);
    void Animate(bool in);
    void Pulse(Item const& it);
    void Click(Item const& it);
    void DeferChord(WORD key, bool shift);
    void DragTo(float x);
    Item const* HitAt(float x, float y) const;
    float TextWidth(std::wstring const& t, IDWriteTextFormat* f);

    LRESULT PanelProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT HotProc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK PanelThunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK HotThunk(HWND, UINT, WPARAM, LPARAM);
};

// ---------------------------------------------------------------------------------------------
// windows

void ControlCenter::Impl::RegisterClasses() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = PanelThunk;
    wc.lpszClassName = kPanelClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = HotThunk;
    wc.lpszClassName = kHotClass;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    RegisterClassExW(&wc);
}

// A 2 px strip on the right edge of the primary monitor, from just below the caption-button row down
// a third of the screen, so it never covers a maximized window's close button. Nearly invisible
// (alpha 1), non-activating. Resting in its top 100 px opens the panel after a dwell; a fast flick into
// it, or a touch swipe in from it, opens the panel anywhere along it.
void ControlCenter::Impl::PlaceHotZone() {
    HMONITOR m = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(m, &mi);
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dpi, &dy);
    float k = dpi / 96.0f;
    int w = std::max(2, static_cast<int>(2 * k)), y = mi.rcMonitor.top + static_cast<int>(36 * k);
    int h = std::max<int>(static_cast<int>(132 * k), (mi.rcMonitor.bottom - mi.rcMonitor.top) / 3);
    if (!hot) {
        hot = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kHotClass,
                              L"HaloControlCenterHotZone", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (!hot) return;
        SetLayeredWindowAttributes(hot, 0, 1, LWA_ALPHA);
    }
    SetWindowPos(hot, HWND_TOPMOST, mi.rcMonitor.right - w, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

LRESULT CALLBACK ControlCenter::Impl::HotThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->HotProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

// True when the pointer arrived at this message fast and from the left: >= 1.2 px/ms over the last
// 100 ms, at least 60 px of travel, mostly horizontal. Uses the system's mouse-move history, so it is
// event-driven (no polling and no hook).
bool ControlCenter::Impl::FlickedIn() {
    DWORD pos = GetMessagePos();
    MOUSEMOVEPOINT cur{static_cast<int>(static_cast<short>(LOWORD(pos))), static_cast<int>(static_cast<short>(HIWORD(pos))),
                       static_cast<DWORD>(GetMessageTime()), 0};
    MOUSEMOVEPOINT hist[16]{};
    int n = GetMouseMovePointsEx(sizeof(cur), &cur, hist, 16, GMMP_USE_DISPLAY_POINTS);
    if (n < 2) return false;
    int oldest = 0;
    for (int i = 1; i < n; i++) {
        if (static_cast<int>(hist[0].time - hist[i].time) > 100) break;
        oldest = i;
    }
    if (!oldest) return false;
    float dt = static_cast<float>(std::max<DWORD>(1, hist[0].time - hist[oldest].time));
    int dx = static_cast<short>(hist[0].x) - static_cast<short>(hist[oldest].x);
    int dy = std::abs(static_cast<short>(hist[0].y) - static_cast<short>(hist[oldest].y));
    return dx >= 60 && dx > dy && dx / dt >= 1.2f;
}

// Event-driven: the pointer entering the strip arms one dwell timer; leaving disarms it.
LRESULT ControlCenter::Impl::HotProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (st == St::Closed && FlickedIn()) { Open(); return 0; }
            if (!hotTracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                hotTracking = TrackMouseEvent(&tme);
                RECT r;
                GetWindowRect(h, &r);
                POINT p;
                GetCursorPos(&p);
                if (p.y - r.top < 100 * s) SetTimer(h, kTimerHot, kHotDwellMs, nullptr);  // dwell only near the corner
            }
            return 0;
        case WM_MOUSELEAVE:
            hotTracking = false;
            KillTimer(h, kTimerHot);
            return 0;
        // Touch: a finger that lands on the strip and drags left by 40 px. (Windows' own edge-swipe
        // gesture may claim the touch first; see the report.)
        case WM_POINTERDOWN:
        case WM_POINTERUPDATE: {
            POINTER_INFO pi{};
            if (!GetPointerInfo(GET_POINTERID_WPARAM(w), &pi) || pi.pointerType != PT_TOUCH) break;
            if (m == WM_POINTERDOWN) touchStart = pi.ptPixelLocation;
            else if (IS_POINTER_INCONTACT_WPARAM(w) && touchStart.x >= 0 && touchStart.x - pi.ptPixelLocation.x >= 40 * s &&
                     st == St::Closed) {
                touchStart = {-1, -1};
                Open();
            }
            return 0;
        }
        case WM_POINTERUP: touchStart = {-1, -1}; break;
        case WM_TIMER:
            if (w == kTimerHot) {
                KillTimer(h, kTimerHot);
                if (st == St::Closed) Open();
            } else if (w == kTimerChord) {
                KillTimer(h, kTimerChord);
                svc::SendWinChord(chordKey, chordShift);
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK ControlCenter::Impl::PanelThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, m, w, l);
    try {
        return self->PanelProc(h, m, w, l);
    } catch (winrt::hresult_error const& e) {  // a failed WinRT call must never take the dock down
        svc::Log(L"panel message " + std::to_wstring(m) + L" failed: " + std::wstring(e.message().c_str()));
        return DefWindowProcW(h, m, w, l);
    }
}

Item const* ControlCenter::Impl::HitAt(float x, float y) const {
    for (auto it = items.rbegin(); it != items.rend(); ++it)
        if (!Passive(it->id) && In(it->r, x, y)) return &*it;
    return nullptr;
}

LRESULT ControlCenter::Impl::PanelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const float x = static_cast<short>(LOWORD(l)) / s, y = static_cast<short>(HIWORD(l)) / s;
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && st == St::Open) Close();  // clicked outside
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) Close();
            return 0;
        case WM_LBUTTONDOWN:
            if (st != St::Open) return 0;
            if (auto* it = HitAt(x, y)) {
                if (it->id == Id::Bright || it->id == Id::Vol) {
                    drag = it->id;
                    SetCapture(hwnd);
                    DragTo(x);
                } else {
                    pressed = *it;
                    Render();
                }
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (drag != Id::None) { DragTo(x); return 0; }
            if (!tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                tracking = TrackMouseEvent(&tme);
            }
            auto* it = HitAt(x, y);
            Item now = it ? *it : Item{};
            if (!Same(now, hover)) { hover = now; Render(); }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            if (hover.id != Id::None) { hover = {}; Render(); }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (drag != Id::None) {
                if (drag == Id::Bright && bright >= 0) svc::brightness::Set(bright);  // the final value always lands
                drag = Id::None;
                if (m == WM_LBUTTONUP) ReleaseCapture();
            } else if (pressed.id != Id::None) {
                Item p = std::exchange(pressed, Item{});
                auto* it = m == WM_LBUTTONUP ? HitAt(x, y) : nullptr;
                if (it && Same(*it, p)) Click(*it);
                else Render();
            }
            return 0;
        case kMsgAudio: SyncSpeaker(); Render(); return 0;
        case kMsgMic: SyncMic(); Render(); return 0;
        case kMsgRadios: SyncRadios(); Render(); return 0;
        case kMsgBright:
            if (svc::brightness::Supported()) bright = svc::brightness::Level();
            Render();
            return 0;
        case kMsgMedia: media.Pull(); return 0;
        case kMsgWifi: case kMsgBt: Render(); return 0;
        case WM_TIMER:
            if (w == kTimerClose) Teardown();
            return 0;
        case WM_NCDESTROY: hwnd = nullptr; break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// open / close

void ControlCenter::Impl::Open() {
    if (st == St::Open) return;
    if (st == St::Closing) Teardown();

    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR hm = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    mon = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s = dx / 96.0f;

    try {
        BuildFonts();
        hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass,
                               L"Halo Control Center", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
        if (!hwnd) return;
        BOOL on = TRUE;
        DwmSetWindowAttribute(hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));
        SyncAll();
        Layout();
        int wPx = static_cast<int>(std::ceil(kW * s)), hPx = static_cast<int>(std::ceil(height * s));
        rect = {mon.right - wPx - static_cast<int>(kMargin * s), mon.top + static_cast<int>(kMargin * s), 0, 0};
        rect.right = rect.left + wPx;
        rect.bottom = rect.top + hPx;
        SetWindowPos(hwnd, HWND_TOPMOST, rect.left, rect.top, wPx, hPx, SWP_NOACTIVATE);
        BuildVisuals();
        Render();
        pill.Opacity(0);
        st = St::Open;
        ShowWindow(hwnd, SW_SHOW);
        Focus();
        Animate(true);
        // Async sources fill in as they arrive.
        radios.Start(hwnd, kMsgRadios);
        media.Start(hwnd, kMsgMedia, [this] { if (st == St::Open) Render(); });
        svc::brightness::Probe(hwnd, kMsgBright);
        if (api->onShown) api->onShown();
    } catch (winrt::hresult_error const& e) {
        svc::Log(L"open failed: " + std::wstring(e.message().c_str()));
        Teardown();
    }
}

// Win+A arrives from a hook thread, so Windows may refuse the foreground change; borrowing the
// foreground thread's input queue is the standard way around that.
void ControlCenter::Impl::Focus() {
    DWORD me = GetCurrentThreadId(), fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    bool attached = fg && fg != me && AttachThreadInput(me, fg, TRUE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (attached) AttachThreadInput(me, fg, FALSE);
    svc::Log(GetForegroundWindow() == hwnd ? L"panel focused" : L"panel could NOT take focus");
}

void ControlCenter::Impl::Close() {
    if (st != St::Open) return;
    st = St::Closing;
    // Nothing listens while the panel is hidden.
    speaker.Unbind();
    mic.Unbind();
    radios.Stop();
    media.Stop();
    svc::WifiStop();
    svc::BtStop();
    Animate(false);
    SetTimer(hwnd, kTimerClose, kCloseMs, nullptr);
    if (api->onHidden) api->onHidden();
}

// Releases the window and every composition and DirectWrite object.
void ControlCenter::Impl::Teardown() {
    bool wasOpen = st == St::Open;
    speaker.Unbind();
    mic.Unbind();
    radios.Stop();
    media.Stop();
    svc::WifiStop();
    svc::BtStop();
    drag = Id::None;
    outsOpen = false;
    expand = Expand::None;
    hover = pressed = {};
    tracking = false;
    if (hwnd) { KillTimer(hwnd, kTimerClose); DestroyWindow(hwnd); }
    hwnd = nullptr;
    if (surf) surf.Close();
    surf = nullptr; content = nullptr; pill = nullptr; fx = nullptr; root = nullptr; target = nullptr; clip = nullptr; rim = nullptr;
    fTime = fDate = fTitle = fSub = fSubR = fBatt = fIcon = fIconS = fPill = fCap = nullptr;
    dw = nullptr;
    items.clear();
    st = St::Closed;
    rect = {};
    if (wasOpen && api->onHidden) api->onHidden();
}

void ControlCenter::Impl::Animate(bool in) {
    pill.CenterPoint({kW * s, 0, 0});
    if (in) {
        auto fade = comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, 1.0f);
        fade.Duration(140ms);
        pill.StartAnimation(L"Opacity", fade);
        auto scale = comp.CreateSpringVector3Animation();
        scale.InitialValue(Ref3(0.94f));
        scale.FinalValue(Ref3(1.0f));
        scale.DampingRatio(0.74f);
        scale.Period(52ms);
        pill.StartAnimation(L"Scale", scale);
        auto slide = comp.CreateSpringScalarAnimation();
        slide.InitialValue(-10.0f * s);
        slide.FinalValue(0.0f);
        slide.DampingRatio(0.74f);
        slide.Period(52ms);
        pill.StartAnimation(L"Offset.Y", slide);
    } else {
        auto fade = comp.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(1.0f, 0.0f);
        fade.Duration(std::chrono::milliseconds(kCloseMs - 10));
        pill.StartAnimation(L"Opacity", fade);
        auto scale = comp.CreateVector3KeyFrameAnimation();
        scale.InsertKeyFrame(1.0f, {0.97f, 0.97f, 1.0f});
        scale.Duration(std::chrono::milliseconds(kCloseMs - 10));
        pill.StartAnimation(L"Scale", scale);
    }
}

// A soft ring that springs out of a toggle's icon: the "springy" feedback for a click. One
// compositor animation, no timer; the next pulse replaces it.
void ControlCenter::Impl::Pulse(Item const& it) {
    if (!fx) return;
    fx.Children().RemoveAll();
    auto geo = comp.CreateEllipseGeometry();
    geo.Radius({15 * s, 15 * s});
    auto shape = comp.CreateSpriteShape(geo);
    shape.StrokeBrush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0xB0, 0xFF, 0xFF, 0xFF)));
    shape.StrokeThickness(2 * s);
    auto v = comp.CreateShapeVisual();
    v.Size({1, 1});
    v.Offset({it.px * s, it.py * s, 0});
    v.Shapes().Append(shape);
    fx.Children().InsertAtTop(v);
    auto sc = comp.CreateVector3KeyFrameAnimation();
    sc.InsertKeyFrame(0.0f, {0.85f, 0.85f, 1.0f});
    sc.InsertKeyFrame(1.0f, {1.7f, 1.7f, 1.0f}, comp.CreateCubicBezierEasingFunction({0.2f, 0.8f}, {0.3f, 1.0f}));
    sc.Duration(380ms);
    v.StartAnimation(L"Scale", sc);
    auto op = comp.CreateScalarKeyFrameAnimation();
    op.InsertKeyFrame(0.0f, 1.0f);
    op.InsertKeyFrame(1.0f, 0.0f);
    op.Duration(380ms);
    v.StartAnimation(L"Opacity", op);
}

// ---------------------------------------------------------------------------------------------
// state

void ControlCenter::Impl::SyncSpeaker() {
    volume = speaker.Available() ? speaker.Volume() : 0;
    muted = speaker.Available() && speaker.Muted();
}
void ControlCenter::Impl::SyncMic() { micMuted = mic.Available() && mic.Muted(); }
void ControlCenter::Impl::SyncRadios() {
    wifiOn = radios.WifiOn();
    btOn = radios.BluetoothOn();
    ssid = svc::ConnectedSsid();
}

void ControlCenter::Impl::SyncAll() {
    speaker.Bind(eRender, hwnd, kMsgAudio);
    mic.Bind(eCapture, hwnd, kMsgMic);
    SyncSpeaker();
    SyncMic();
    outs = svc::OutputDevices();
    if (outs.size() > kMaxOutputs) outs.resize(kMaxOutputs);
    battery = svc::ReadBattery();
    mode = svc::ReadPowerMode();
    dnd = svc::ReadDnd();
    dark = svc::ReadDarkMode();
    energy = svc::ReadEnergySaver();
    hotspotAvail = svc::HotspotAvailable();
    hotspotOn = hotspotAvail && svc::ReadHotspot();
    ssid = svc::ConnectedSsid();
    bright = svc::brightness::Supported() ? svc::brightness::Level() : -1;
}

// The shell's own chords go out after the panel has faded, so it isn't in the snip.
void ControlCenter::Impl::DeferChord(WORD key, bool shift) {
    chordKey = key;
    chordShift = shift;
    Close();
    if (hot) SetTimer(hot, kTimerChord, 280, nullptr);
    else svc::SendWinChord(key, shift);
}

void ControlCenter::Impl::Click(Item const& it) {
    switch (it.id) {
        case Id::Power:
            mode = svc::NextPowerMode(mode);
            svc::SetPowerMode(mode);
            break;
        case Id::Wifi:
            if (!radios.HasWifi()) return;
            wifiOn = !wifiOn;
            radios.SetWifi(wifiOn);
            Pulse(it);
            break;
        case Id::Bluetooth:
            if (!radios.HasBluetooth()) return;
            btOn = !btOn;
            radios.SetBluetooth(btOn);
            Pulse(it);
            break;
        case Id::Airplane:
            if (!radios.HasWifi() && !radios.HasBluetooth()) return;
            if (wifiOn || btOn) {  // go to airplane mode, remembering what was on
                savedWifi = wifiOn;
                savedBt = btOn;
                if (radios.HasWifi()) radios.SetWifi(false);
                if (radios.HasBluetooth()) radios.SetBluetooth(false);
                wifiOn = btOn = false;
            } else {
                wifiOn = savedWifi || !radios.HasBluetooth();
                btOn = savedBt;
                if (radios.HasWifi()) radios.SetWifi(wifiOn);
                if (radios.HasBluetooth()) radios.SetBluetooth(btOn);
            }
            Pulse(it);
            break;
        case Id::Hotspot:
            if (!hotspotAvail) return;
            hotspotOn = !hotspotOn;
            svc::SetHotspot(hotspotOn);
            Pulse(it);
            break;
        case Id::WifiChev:
            expand = expand == Expand::Wifi ? Expand::None : Expand::Wifi;
            if (expand == Expand::Wifi) svc::WifiRefresh(hwnd, kMsgWifi);
            break;
        case Id::BtChev:
            expand = expand == Expand::Bluetooth ? Expand::None : Expand::Bluetooth;
            if (expand == Expand::Bluetooth) svc::BtRefresh(hwnd, kMsgBt);
            break;
        case Id::WifiNet:
            svc::WifiConnect(svc::WifiNets()[it.arg].ssid);
            expand = Expand::None;
            break;
        case Id::BtDev: svc::OpenSettings(L"ms-settings:bluetooth"); break;  // Windows has no public "connect" API
        case Id::Dnd:
            dnd = !dnd;
            svc::SetDnd(dnd);
            Pulse(it);
            break;
        case Id::Dark:
            dark = !dark;
            svc::SetDarkMode(dark);
            Pulse(it);
            break;
        case Id::Night: svc::OpenSettings(L"ms-settings:nightlight"); Pulse(it); break;
        case Id::Energy: svc::OpenSettings(L"ms-settings:batterysaver"); Pulse(it); break;
        case Id::Mic:
            if (!mic.Available()) return;
            micMuted = !micMuted;
            mic.SetMute(micMuted);
            Pulse(it);
            break;
        case Id::Snip: Pulse(it); DeferChord('S', true); return;
        case Id::Cast: Pulse(it); DeferChord('K', false); return;
        case Id::Project: Pulse(it); DeferChord('P', false); return;
        case Id::OutRow: outsOpen = !outsOpen; break;
        case Id::OutDev: {
            auto id = outs[it.arg].id;
            svc::SetDefaultOutput(id);
            for (auto& d : outs) d.isDefault = d.id == id;
            outsOpen = false;
            if (!svc::DryRun()) { speaker.Bind(eRender, hwnd, kMsgAudio); SyncSpeaker(); }
            break;
        }
        case Id::Prev: media.Prev(); break;
        case Id::PlayPause: media.PlayPause(); break;
        case Id::Next: media.Next(); break;
        default: return;
    }
    Render();
}

void ControlCenter::Impl::DragTo(float x) {
    for (auto const& it : items) {
        if (it.id != drag) continue;
        float h = it.r.bottom - it.r.top;
        float f = std::clamp((x - it.r.left - h / 2) / (it.r.right - it.r.left - h), 0.0f, 1.0f);
        if (drag == Id::Vol) {
            volume = f;
            speaker.SetVolume(f);
        } else {
            bright = static_cast<int>(std::lround(f * 100));
            ULONGLONG now = GetTickCount64();
            if (now - lastBrightSet >= 60) { lastBrightSet = now; svc::brightness::Set(bright); }
        }
        Render();
        return;
    }
}

// ---------------------------------------------------------------------------------------------
// layout + drawing

float ControlCenter::Impl::TextWidth(std::wstring const& t, IDWriteTextFormat* f) {
    winrt::com_ptr<IDWriteTextLayout> l;
    DWRITE_TEXT_METRICS tm{};
    if (SUCCEEDED(dw->CreateTextLayout(t.c_str(), static_cast<UINT32>(t.size()), f, 1000, 100, l.put()))) l->GetMetrics(&tm);
    return tm.widthIncludingTrailingWhitespace;
}

void ControlCenter::Impl::Layout() {
    items.clear();
    const float x0 = kPad, x1 = kW - kPad;
    float y = kPad;
    auto add = [&](Id id, D2D1_RECT_F r, int arg = 0, float px = 0, float py = 0) { items.push_back({id, r, arg, px, py}); };

    add(Id::Header, {x0, y, x1, y + 62});
    if (mode != svc::PowerMode::Unknown) {
        float pw = TextWidth(svc::PowerModeName(mode), fPill.get()) + 22;
        float py = y + (battery.hasBattery ? 34 : 20);
        add(Id::Power, {x1 - pw, py, x1, py + 24});
    }
    y += 62 + kGap;

    // Connectivity group (left) and Focus + two small tiles (right).
    add(Id::ConnTile, {x0, y, x0 + kCol, y + 150});
    struct Row { Id id, chev; };
    Row rows[] = {{Id::Wifi, Id::WifiChev}, {Id::Bluetooth, Id::BtChev}, {Id::Airplane, Id::None}, {Id::Hotspot, Id::None}};
    for (int i = 0; i < 4; i++) {
        float ry = y + 7 + i * 34, cy = ry + 17;
        D2D1_RECT_F full{x0 + 4, ry, x0 + kCol - 4, ry + 34};
        if (rows[i].chev != Id::None) {
            add(rows[i].id, {full.left, ry, full.right - 28, ry + 34}, 0, x0 + 22, cy);
            add(rows[i].chev, {full.right - 28, ry, full.right, ry + 34});
        } else {
            add(rows[i].id, full, 0, x0 + 22, cy);
        }
    }
    const float rx = x0 + kCol + kGap;
    add(Id::Dnd, {rx, y, x1, y + 69}, 0, rx + 28, y + 34.5f);
    add(Id::Dark, {rx, y + 81, rx + 72, y + 150}, 0, rx + 36, y + 81 + 25);
    add(Id::Night, {x1 - 72, y + 81, x1, y + 150}, 0, x1 - 36, y + 81 + 25);
    y += 150 + kGap;

    // Inline lists under the connectivity group.
    if (expand != Expand::None) {
        bool wifi = expand == Expand::Wifi;
        size_t n = wifi ? svc::WifiNets().size() : svc::BtDevices().size();
        float h = 36 + std::max<size_t>(1, n) * 30 + 4;
        add(Id::ListTile, {x0, y, x1, y + h});
        for (size_t i = 0; i < n; i++)
            add(wifi ? Id::WifiNet : Id::BtDev, {x0 + 6, y + 36 + i * 30, x1 - 6, y + 66 + i * 30}, static_cast<int>(i));
        y += h + kGap;
    }

    // Five small tiles.
    {
        Id ids[] = {Id::Mic, Id::Energy, Id::Snip, Id::Cast, Id::Project};
        float tw = (x1 - x0 - 4 * 10) / 5;
        for (int i = 0; i < 5; i++) {
            float tx = x0 + i * (tw + 10);
            add(ids[i], {tx, y, tx + tw, y + 66}, 0, tx + tw / 2, y + 24);
        }
        y += 66 + kGap;
    }

    if (bright >= 0) {
        add(Id::DisplayTile, {x0, y, x1, y + 66});
        add(Id::Bright, {x0 + 12, y + 34, x1 - 12, y + 58});
        y += 66 + kGap;
    }
    if (speaker.Available()) {
        float extra = outsOpen ? outs.size() * 30.0f + 4 : 0;
        add(Id::SoundTile, {x0, y, x1, y + 66 + extra});
        add(Id::OutRow, {x0 + 70, y + 6, x1 - 8, y + 30});
        add(Id::Vol, {x0 + 12, y + 34, x1 - 12, y + 58});
        if (outsOpen)
            for (size_t i = 0; i < outs.size(); i++)
                add(Id::OutDev, {x0 + 6, y + 64 + i * 30, x1 - 6, y + 94 + i * 30}, static_cast<int>(i));
        y += 66 + extra + kGap;
    }
    if (media.State().has) {
        add(Id::MediaTile, {x0, y, x1, y + 92});
        add(Id::Prev, {x0 + 88, y + 50, x0 + 128, y + 84});
        add(Id::PlayPause, {x0 + 128, y + 50, x0 + 168, y + 84});
        add(Id::Next, {x0 + 168, y + 50, x0 + 208, y + 84});
        y += 92 + kGap;
    }
    height = std::min({y, kMaxH, std::max(200.0f, (mon.bottom - mon.top) / s - 2 * kMargin)});
}

// Window, visuals and surface all follow `height`.
void ControlCenter::Impl::ApplySize() {
    int wPx = static_cast<int>(std::ceil(kW * s)), hPx = static_cast<int>(std::ceil(height * s));
    if (rect.bottom - rect.top == hPx) return;
    rect.bottom = rect.top + hPx;
    SetWindowPos(hwnd, nullptr, 0, 0, wPx, hPx, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pill.Size({static_cast<float>(wPx), static_cast<float>(hPx)});
    content.Size({static_cast<float>(wPx), static_cast<float>(hPx)});
    clip.Size({static_cast<float>(wPx), static_cast<float>(hPx)});
    rim.Size({wPx - s, hPx - s});
    surf.as<ICompositionDrawingSurfaceInterop>()->Resize({wPx, hPx});
    if (st == St::Open && api->onResized) api->onResized();
}

void ControlCenter::Impl::Render() {
    if (!surf || st == St::Closed) return;
    Layout();
    ApplySize();
    auto interop = surf.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT off{};
    if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return;
    try {
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(static_cast<float>(off.x), static_cast<float>(off.y)));
        dc->Clear(ColorF(0, 0.0f));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        winrt::com_ptr<ID2D1SolidColorBrush> br;
        dc->CreateSolidColorBrush(kText, br.put());
        // Tiles get a soft top-lit gradient and a hairline edge: the "inner highlight".
        winrt::com_ptr<ID2D1GradientStopCollection> stops;
        winrt::com_ptr<ID2D1LinearGradientBrush> lg;
        D2D1_GRADIENT_STOP gs[2] = {{0.0f, ColorF(1, 1, 1, 0.15f)}, {1.0f, ColorF(1, 1, 1, 0.075f)}};
        dc->CreateGradientStopCollection(gs, 2, stops.put());
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {0, 1}), stops.get(), lg.put());

        auto box = [&](D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
            br->SetColor(c);
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br.get());
        };
        auto tile = [&](D2D1_RECT_F r, float rad) {
            lg->SetStartPoint({0, r.top});
            lg->SetEndPoint({0, r.bottom});
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg.get());
            br->SetColor(ColorF(1, 1, 1, 0.09f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect({r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, rad, rad), br.get(), 1.0f);
        };
        auto txt = [&](std::wstring const& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
            br->SetColor(c);
            dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, r, br.get());
        };
        auto glyph = [&](wchar_t g, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) { txt(std::wstring(1, g), f, r, c); };
        auto circle = [&](float cx, float cy, float rad, D2D1_COLOR_F c) {
            br->SetColor(c);
            dc->FillEllipse(D2D1::Ellipse({cx, cy}, rad, rad), br.get());
        };
        // Hover and press states over a tile or row.
        auto state = [&](Item const& it, float rad) {
            if (Same(it, pressed)) box(it.r, rad, ColorF(0, 0, 0, 0.16f));
            else if (Same(it, hover)) box(it.r, rad, ColorF(1, 1, 1, 0.07f));
        };
        // An icon circle: accent when on, glass when off; a glyph, or a custom mark for dark mode.
        auto icon = [&](Item const& it, float cx, float cy, float rad, bool on, bool avail, wchar_t g) {
            float a = avail ? 1.0f : 0.45f;
            circle(cx, cy, rad, on ? ColorF(0x0A84FF, a) : ColorF(1, 1, 1, 0.17f * a));
            if (it.id == Id::Dark) {  // half-filled disc
                br->SetColor(ColorF(1, 1, 1, a));
                dc->DrawEllipse(D2D1::Ellipse({cx, cy}, 7, 7), br.get(), 1.6f);
                winrt::com_ptr<ID2D1Factory> f;
                dc->GetFactory(f.put());
                winrt::com_ptr<ID2D1PathGeometry> pg;
                winrt::com_ptr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(f->CreatePathGeometry(pg.put())) && SUCCEEDED(pg->Open(sink.put()))) {
                    sink->BeginFigure({cx, cy - 7}, D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddArc(D2D1::ArcSegment({cx, cy + 7}, {7, 7}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sink->Close();
                    dc->FillGeometry(pg.get(), br.get());
                }
            } else {
                glyph(g, fIconS.get(), {cx - rad, cy - rad, cx + rad, cy + rad}, ColorF(1, 1, 1, a));
            }
        };

        for (auto const& it : items) {
            auto const& r = it.r;
            switch (it.id) {
                case Id::Header: {
                    SYSTEMTIME lt{};
                    GetLocalTime(&lt);
                    wchar_t t[32] = {}, d[64] = {};
                    GetTimeFormatEx(nullptr, TIME_NOSECONDS, &lt, nullptr, t, 32);
                    GetDateFormatEx(nullptr, 0, &lt, L"dddd, d MMMM", d, 64, nullptr);
                    txt(t, fTime.get(), {r.left + 2, r.top - 4, r.right - 124, r.top + 40}, kText);
                    txt(d, fDate.get(), {r.left + 3, r.top + 40, r.right - 124, r.top + 60}, kDim);
                    if (battery.hasBattery) {
                        int lvl = std::min(10, battery.percent / 10);
                        wchar_t g = static_cast<wchar_t>((battery.ac ? 0xEBAB : 0xEBA0) + lvl);
                        std::wstring pct = std::to_wstring(battery.percent) + L"%";
                        float pw = TextWidth(pct, fBatt.get());
                        txt(pct, fBatt.get(), {r.right - pw - 30, r.top + 4, r.right, r.top + 28}, kText);
                        glyph(g, fIcon.get(), {r.right - pw - 42, r.top + 4, r.right - pw - 6, r.top + 28}, kText);
                    }
                    break;
                }
                case Id::Power:
                    tile(r, 12);
                    state(it, 12);
                    txt(svc::PowerModeName(mode), fPill.get(), r, kText);
                    break;
                case Id::ConnTile: case Id::DisplayTile: case Id::SoundTile: case Id::ListTile: case Id::MediaTile:
                    tile(r, kTileR);
                    if (it.id == Id::DisplayTile) txt(L"Display", fTitle.get(), {r.left + 14, r.top + 9, r.right - 14, r.top + 29}, kText);
                    if (it.id == Id::SoundTile) txt(L"Sound", fTitle.get(), {r.left + 14, r.top + 9, r.left + 80, r.top + 29}, kText);
                    if (it.id == Id::ListTile)
                        txt(expand == Expand::Wifi ? L"Known Wi-Fi networks" : L"Paired Bluetooth devices", fTitle.get(),
                            {r.left + 14, r.top + 10, r.right - 14, r.top + 30}, kText);
                    break;
                case Id::Wifi: case Id::Bluetooth: case Id::Airplane: case Id::Hotspot: case Id::Dnd: {
                    bool avail = true, on = false;
                    wchar_t g = 0;
                    std::wstring title, sub;
                    if (it.id == Id::Wifi) {
                        avail = radios.HasWifi(); on = wifiOn; g = 0xE701; title = L"Wi-Fi";
                        sub = !avail ? L"Unavailable" : !on ? L"Off" : ssid.empty() ? L"Not connected" : ssid;
                    } else if (it.id == Id::Bluetooth) {
                        avail = radios.HasBluetooth(); on = btOn; g = 0xE702; title = L"Bluetooth";
                        sub = !avail ? L"Unavailable" : on ? L"On" : L"Off";
                    } else if (it.id == Id::Airplane) {
                        avail = radios.HasWifi() || radios.HasBluetooth(); on = avail && !wifiOn && !btOn; g = 0xE709;
                        title = L"Airplane Mode"; sub = on ? L"On" : L"Off";
                    } else if (it.id == Id::Hotspot) {
                        avail = hotspotAvail; on = hotspotOn; g = 0xE704; title = L"Hotspot";
                        sub = !avail ? L"Unavailable" : on ? L"On" : L"Off";
                    } else {
                        on = dnd; g = 0xE708; title = L"Do Not Disturb"; sub = on ? L"On" : L"Off";
                        tile(r, kTileR);
                    }
                    state(it, it.id == Id::Dnd ? kTileR : 10);
                    float a = avail ? 1.0f : 0.45f;
                    icon(it, it.px, it.py, 14.0f + (it.id == Id::Dnd ? 2 : 0), on, avail, g);
                    float tx = it.px + 24, cy = it.py;
                    txt(title, fTitle.get(), {tx, cy - 17, r.right - 4, cy + 1}, ColorF(1, 1, 1, 0.95f * a));
                    txt(sub, fSub.get(), {tx, cy + 1, r.right - 4, cy + 17}, ColorF(1, 1, 1, 0.58f * a));
                    break;
                }
                case Id::WifiChev: case Id::BtChev: {
                    bool open = (it.id == Id::WifiChev && expand == Expand::Wifi) || (it.id == Id::BtChev && expand == Expand::Bluetooth);
                    state(it, 10);
                    glyph(open ? 0xE70E : 0xE76C, fIconS.get(), r, kDim);  // chevron up / right
                    break;
                }
                case Id::Dark: case Id::Night: case Id::Mic: case Id::Energy: case Id::Snip: case Id::Cast: case Id::Project: {
                    bool on = false, avail = true;
                    wchar_t g = 0;
                    std::wstring label;
                    switch (it.id) {
                        case Id::Dark: on = dark; label = L"Dark Mode"; break;
                        case Id::Night: g = 0xE706; label = L"Night Light"; break;
                        case Id::Mic: avail = mic.Available(); on = avail && !micMuted; g = 0xE720; label = micMuted ? L"Muted" : L"Mic"; break;
                        case Id::Energy: on = energy; g = 0xEBAA; label = L"Saver"; break;
                        case Id::Snip: g = 0xE8C6; label = L"Snip"; break;
                        case Id::Cast: g = 0xE7F4; label = L"Cast"; break;
                        default: g = 0xE7F8; label = L"Project"; break;
                    }
                    tile(r, kSmallR);
                    state(it, kSmallR);
                    icon(it, it.px, it.py, 15, on, avail, g);
                    if (it.id == Id::Mic && avail && micMuted) {
                        br->SetColor(ColorF(1, 1, 1, 0.95f));
                        dc->DrawLine({it.px - 7, it.py - 7}, {it.px + 7, it.py + 7}, br.get(), 1.6f);
                    }
                    txt(label, fCap.get(), {r.left, r.top + 43, r.right, r.bottom - 2}, ColorF(1, 1, 1, avail ? 0.78f : 0.4f));
                    break;
                }
                case Id::Bright: case Id::Vol: {
                    float h = r.bottom - r.top, f = it.id == Id::Vol ? volume : bright / 100.0f;
                    box(r, h / 2, kOff);
                    box({r.left, r.top, r.left + h + f * (r.right - r.left - h), r.bottom}, h / 2, kFill);
                    wchar_t g = it.id == Id::Bright ? 0xE706 : (muted || volume <= 0.001f ? 0xE74F : 0xE767);
                    glyph(g, fIconS.get(), {r.left, r.top, r.left + h, r.bottom}, kOnFill);
                    break;
                }
                case Id::OutRow: {
                    std::wstring name = L"Output";
                    for (auto const& d : outs) if (d.isDefault) name = d.name;
                    state(it, 8);
                    txt(name, fSubR.get(), {r.left, r.top + 4, r.right - 24, r.bottom}, kDim);
                    glyph(outsOpen ? 0xE70E : 0xE70D, fIconS.get(), {r.right - 24, r.top, r.right, r.bottom}, kDim);
                    break;
                }
                case Id::OutDev: case Id::WifiNet: case Id::BtDev: {
                    std::wstring name;
                    bool mark = false;
                    std::wstring sub;
                    if (it.id == Id::OutDev) { name = outs[it.arg].name; mark = outs[it.arg].isDefault; }
                    else if (it.id == Id::WifiNet) {
                        auto const& n = svc::WifiNets()[it.arg];
                        name = n.ssid; mark = n.connected;
                        sub = n.connected ? L"Connected" : L"";
                    } else {
                        auto const& d = svc::BtDevices()[it.arg];
                        name = d.name; mark = d.connected; sub = d.connected ? L"Connected" : L"";
                    }
                    state(it, 8);
                    if (mark) box({r.left, r.top + 1, r.right, r.bottom - 1}, 8, ColorF(1, 1, 1, 0.07f));
                    txt(name, fSub.get(), {r.left + 10, r.top + 7, r.right - (sub.empty() ? 34 : 110), r.bottom}, mark ? kText : kDim);
                    if (!sub.empty()) txt(sub, fSubR.get(), {r.right - 100, r.top + 7, r.right - 34, r.bottom}, kDim);
                    if (mark) glyph(0xE73E, fIconS.get(), {r.right - 30, r.top, r.right - 6, r.bottom}, kAccent);
                    break;
                }
                case Id::Prev: case Id::PlayPause: case Id::Next: {
                    auto const& ms = media.State();
                    bool ok = it.id == Id::Prev ? ms.canPrev : it.id == Id::Next ? ms.canNext : true;
                    wchar_t g = it.id == Id::Prev ? 0xE892 : it.id == Id::Next ? 0xE893 : (ms.playing ? 0xE769 : 0xE768);
                    state(it, 10);
                    glyph(g, fIcon.get(), r, ColorF(1, 1, 1, ok ? 0.95f : 0.3f));
                    break;
                }
                default: break;
            }
            if (it.id == Id::MediaTile) {
                auto const& ms = media.State();
                D2D1_RECT_F art{r.left + 14, r.top + 14, r.left + 78, r.top + 78};
                if (ms.artSize > 0 && ms.art.size() == static_cast<size_t>(ms.artSize) * ms.artSize) {
                    winrt::com_ptr<ID2D1Bitmap> bmp;
                    auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    winrt::com_ptr<ID2D1BitmapBrush> bb;
                    if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(ms.artSize, ms.artSize), ms.art.data(), ms.artSize * 4, &props, bmp.put())) &&
                        SUCCEEDED(dc->CreateBitmapBrush(bmp.get(), bb.put()))) {
                        float k = 64.0f / ms.artSize;
                        bb->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(art.left, art.top));
                        bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        dc->FillRoundedRectangle(D2D1::RoundedRect(art, 10, 10), bb.get());
                    }
                } else {
                    box(art, 10, kOff);
                    glyph(0xE8D6, fIcon.get(), art, kDim);  // music note
                }
                txt(ms.title.empty() ? L"Now Playing" : ms.title, fTitle.get(), {r.left + 90, r.top + 12, r.right - 12, r.top + 30}, kText);
                txt(ms.artist, fSub.get(), {r.left + 90, r.top + 30, r.right - 12, r.top + 48}, kDim);
            }
        }
    } catch (...) {}
    interop->EndDraw();
}

// ---------------------------------------------------------------------------------------------
// building blocks

void ControlCenter::Impl::BuildFonts() {
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.put())));
    auto mk = [&](winrt::com_ptr<IDWriteTextFormat>& out, wchar_t const* family, DWRITE_FONT_WEIGHT w, float size,
                  DWRITE_TEXT_ALIGNMENT al, DWRITE_PARAGRAPH_ALIGNMENT pa) {
        winrt::check_hresult(dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out.put()));
        out->SetTextAlignment(al);
        out->SetParagraphAlignment(pa);
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        winrt::com_ptr<IDWriteInlineObject> sign;
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(out.get(), sign.put()))) out->SetTrimming(&trim, sign.get());
    };
    const auto L = DWRITE_TEXT_ALIGNMENT_LEADING, C = DWRITE_TEXT_ALIGNMENT_CENTER, T = DWRITE_TEXT_ALIGNMENT_TRAILING;
    const auto N = DWRITE_PARAGRAPH_ALIGNMENT_NEAR, M = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    const auto SB = DWRITE_FONT_WEIGHT_SEMI_BOLD, R = DWRITE_FONT_WEIGHT_NORMAL;
    // Display for the big number, Text for labels: the SF Pro Display / Text split.
    mk(fTime, L"Segoe UI Variable Display", SB, 34, L, N);
    mk(fDate, L"Segoe UI Variable Text", R, 12.5f, L, N);
    mk(fTitle, L"Segoe UI Variable Text", SB, 12.5f, L, N);
    mk(fSub, L"Segoe UI Variable Text", R, 11.5f, L, N);
    mk(fSubR, L"Segoe UI Variable Text", R, 11.5f, T, N);
    mk(fBatt, L"Segoe UI Variable Text", SB, 14, T, M);
    mk(fIcon, L"Segoe Fluent Icons", R, 17, C, M);
    mk(fIconS, L"Segoe Fluent Icons", R, 14, C, M);
    mk(fPill, L"Segoe UI Variable Text", SB, 11.5f, C, M);
    mk(fCap, L"Segoe UI Variable Text", R, 11, C, N);
}

// Same recipe as the dock pill (Halo::BuildPill): blurred backdrop, #16161A tint at 50%, 2.5% grain,
// a 1 px gradient rim.
wuc::CompositionBrush ControlCenter::Impl::NoiseBrush(int w, int h) {
    auto surface = gdev.CreateDrawingSurface({static_cast<float>(w), static_cast<float>(h)},
                                             winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                             winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT off{};
    winrt::check_hresult(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off));
    try {
        std::vector<uint32_t> px(static_cast<size_t>(w) * h);
        uint32_t seed = 0x2545F491;
        for (auto& p : px) {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            uint32_t g = seed & 0xFF;
            p = 0xFF000000 | g << 16 | g << 8 | g;
        }
        winrt::com_ptr<ID2D1Bitmap> bmp;
        auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &props, bmp.put()));
        D2D1_RECT_F dest = D2D1::RectF(static_cast<float>(off.x), static_cast<float>(off.y), static_cast<float>(off.x + w), static_cast<float>(off.y + h));
        dc->DrawBitmap(bmp.get(), &dest, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr, nullptr);
    } catch (...) {
        interop->EndDraw();
        throw;
    }
    winrt::check_hresult(interop->EndDraw());
    auto brush = comp.CreateSurfaceBrush(surface);
    brush.Stretch(wuc::CompositionStretch::None);
    return brush;
}

void ControlCenter::Impl::BuildVisuals() {
    const float wPx = std::ceil(kW * s), hPx = std::ceil(height * s);
    winrt::check_hresult(comp.as<ICompositorDesktopInterop>()->CreateDesktopWindowTarget(hwnd, TRUE, winrt::put_abi(target)));
    root = comp.CreateContainerVisual();
    root.RelativeSizeAdjustment({1, 1});
    target.Root(root);
    pill = comp.CreateContainerVisual();
    pill.Size({wPx, hPx});
    root.Children().InsertAtTop(pill);

    auto glass = comp.CreateSpriteVisual();
    glass.RelativeSizeAdjustment({1, 1});
    try {
        auto blur = winrt::make<GaussianBlurEffect>(wuc::CompositionEffectSourceParameter(L"backdrop"), 40.0f);
        auto brush = comp.CreateEffectFactory(blur.as<winrt::Windows::Graphics::Effects::IGraphicsEffect>()).CreateBrush();
        brush.SetSourceParameter(L"backdrop", comp.CreateHostBackdropBrush());
        glass.Brush(brush);
    } catch (...) { svc::Log(L"blur brush failed"); }
    pill.Children().InsertAtTop(glass);

    auto tint = comp.CreateSpriteVisual();
    tint.RelativeSizeAdjustment({1, 1});
    tint.Brush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0x80, 0x16, 0x16, 0x1A)));
    pill.Children().InsertAtTop(tint);

    try {
        auto noise = comp.CreateSpriteVisual();
        noise.RelativeSizeAdjustment({1, 1});
        noise.Opacity(0.025f);
        noise.Brush(NoiseBrush(static_cast<int>(wPx), static_cast<int>(std::ceil(kMaxH * s))));
        pill.Children().InsertAtTop(noise);
    } catch (...) {}

    surf = gdev.CreateDrawingSurface({wPx, hPx}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                     winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    content = comp.CreateSpriteVisual();
    content.Size({wPx, hPx});
    auto sb = comp.CreateSurfaceBrush(surf);
    sb.Stretch(wuc::CompositionStretch::None);
    content.Brush(sb);
    pill.Children().InsertAtTop(content);

    fx = comp.CreateContainerVisual();
    fx.RelativeSizeAdjustment({1, 1});
    pill.Children().InsertAtTop(fx);

    const float t = s;  // 1 px rim, inset by half its width so it stays inside the clip
    rim = comp.CreateRoundedRectangleGeometry();
    rim.Offset({t / 2, t / 2});
    rim.CornerRadius({kRadius * s - t / 2, kRadius * s - t / 2});
    rim.Size({wPx - t, hPx - t});
    auto gradient = comp.CreateLinearGradientBrush();
    gradient.MappingMode(wuc::CompositionMappingMode::Relative);
    gradient.StartPoint({0, 0});
    gradient.EndPoint({0, 1});
    auto stop = [&](float at, uint8_t a) {
        gradient.ColorStops().Append(comp.CreateColorGradientStop(at, winrt::Windows::UI::ColorHelper::FromArgb(a, 0xFF, 0xFF, 0xFF)));
    };
    stop(0.0f, 0x70); stop(0.35f, 0x18); stop(0.7f, 0x0A); stop(1.0f, 0x26);
    auto sprite = comp.CreateSpriteShape(rim);
    sprite.StrokeBrush(gradient);
    sprite.StrokeThickness(t);
    auto rimVisual = comp.CreateShapeVisual();
    rimVisual.RelativeSizeAdjustment({1, 1});
    rimVisual.Shapes().Append(sprite);
    pill.Children().InsertAtTop(rimVisual);

    clip = comp.CreateRoundedRectangleGeometry();
    clip.CornerRadius({kRadius * s, kRadius * s});
    clip.Size({wPx, hPx});
    pill.Clip(comp.CreateGeometricClip(clip));
}

// ---------------------------------------------------------------------------------------------
// public

ControlCenter::ControlCenter(HINSTANCE inst, HWND owner, wuc::Compositor comp, wuc::CompositionGraphicsDevice gdev)
    : m(std::make_unique<Impl>(this, inst, owner, std::move(comp), std::move(gdev))) {
    m->RegisterClasses();
    m->PlaceHotZone();
}

ControlCenter::~ControlCenter() {
    m->Teardown();
    if (m->hot) DestroyWindow(m->hot);
}

void ControlCenter::Toggle() { IsOpen() ? Close() : Open(); }
void ControlCenter::Open() { m->Open(); }
void ControlCenter::Close() { m->Close(); }
bool ControlCenter::IsOpen() const { return m->st == Impl::St::Open; }
void ControlCenter::OnDisplayChange() { m->PlaceHotZone(); }
RECT ControlCenter::PanelRectScreen() const { return m->st == Impl::St::Closed ? RECT{} : m->rect; }

}  // namespace halo
