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
#include <powrprof.h>
#include <thread>
#include <vector>

#include "effects.h"
#include "motion.h"
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

// Grid, in logical px (all drawing is scaled by the monitor DPI): 14 outer inset, 10 between modules,
// 24 panel radius, 18 module radius. The window is the panel plus motion::kPad of empty margin on every
// side, so the spring's overshoot never clips; everything below is in panel ("pill") coordinates.
constexpr float kW = 340, kInset = 14, kGap = 10, kRadius = 24, kTileR = 18;
constexpr float kPillH = 60, kRowH = 68, kSliderH = 52, kMediaH = 88, kHeadH = 76, kActH = 60, kPowH = 48;
constexpr float kMaxH = 900, kMargin = 8;
constexpr int kMaxOutputs = 6;

constexpr UINT kMsgAudio = WM_APP + 1, kMsgMic = WM_APP + 2, kMsgRadios = WM_APP + 3, kMsgBright = WM_APP + 4,
               kMsgMedia = WM_APP + 5, kMsgWifi = WM_APP + 6, kMsgBt = WM_APP + 7, kMsgLate = WM_APP + 8;
constexpr UINT_PTR kTimerClose = 1, kTimerChord = 2, kTimerClock = 3;
constexpr wchar_t kPanelClass[] = L"HaloControlCenter", kHotClass[] = L"HaloControlCenterTimers";

enum class Id {
    None, WifiPill, BtPill, WifiText, BtText, Wifi, Bluetooth, RowTile, Airplane, Hotspot, Night, Dark, Dnd,
    DisplayTile, Bright, SoundTile, Vol, OutRow, OutDev, ListTile, WifiNet, BtDev, MediaTile, Prev, PlayPause, Next,
    Snip, Cast, Settings, Battery, Header, Lock, Power, Mic, PowerTile, PowSleep, PowRestart, PowShut
};
bool Passive(Id id) {
    return id == Id::WifiPill || id == Id::BtPill || id == Id::RowTile || id == Id::DisplayTile || id == Id::SoundTile ||
           id == Id::ListTile || id == Id::MediaTile || id == Id::Header || id == Id::PowerTile;
}
// Controls that squash to 0.96 under the finger (their drawing is lifted onto its own little surface).
bool Scalable(Id id) {
    return id == Id::Wifi || id == Id::Bluetooth || id == Id::Airplane || id == Id::Hotspot || id == Id::Night ||
           id == Id::Dark || id == Id::Dnd || id == Id::Snip || id == Id::Cast || id == Id::Settings ||
           id == Id::Battery || id == Id::Lock || id == Id::Power || id == Id::Mic || id == Id::Prev || id == Id::PlayPause || id == Id::Next;
}
enum class Expand { None, Wifi, Bluetooth };

struct Item {
    Id id = Id::None;
    D2D1_RECT_F r{};
    int arg = 0;
    float rad = 10;  // corner radius of the hover highlight
};
bool Same(Item const& a, Item const& b) { return a.id == b.id && a.arg == b.arg; }

winrt::Windows::Foundation::IReference<wfn::float3> Ref3(float v) {
    return winrt::Windows::Foundation::IReference<wfn::float3>(wfn::float3{v, v, 1.0f});
}
bool In(D2D1_RECT_F const& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

const D2D1_COLOR_F kText = ColorF(1, 1, 1, 0.95f), kDim = ColorF(1, 1, 1, 0.58f), kOff = ColorF(1, 1, 1, 0.17f),
                   kAccent = ColorF(0x0A84FF), kFill = ColorF(1, 1, 1, 0.94f), kOnFill = ColorF(0x1C1C1E);

// Results of the slow reads (hotspot, SSID) done off the UI thread.
struct Late {
    bool hotspotAvail = false, hotspotOn = false;
    std::wstring ssid;
};

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
    RECT mon{}, rect{};  // rect: the panel (pill) on screen, physical px; the window is this plus the margin
    float height = 0;    // panel height, logical
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr}, pill{nullptr}, fx{nullptr};
    wuc::SpriteVisual content{nullptr}, pv{nullptr};
    wuc::ShapeVisual hv{nullptr};
    wuc::CompositionDrawingSurface surf{nullptr}, pvSurf{nullptr};
    wuc::CompositionRoundedRectangleGeometry clip{nullptr}, rim{nullptr}, hvGeo{nullptr};
    bool hvShown = false;

    // Kept across opens: the DirectWrite factory and formats, and the grain texture.
    winrt::com_ptr<IDWriteFactory> dw;
    winrt::com_ptr<IDWriteTextFormat> fTime, fDate, fTitle, fSub, fSubR, fBatt, fIcon, fIconS, fPill, fCap, fBtn;
    wuc::CompositionBrush noiseBrush{nullptr};
    int noiseW = 0;

    // ---- state ----
    svc::AudioEndpoint speaker, mic;
    svc::Radios radios;
    svc::Media media;
    std::vector<svc::Device> outs;
    bool outsOpen = false;
    Expand expand = Expand::None;
    float volume = 0;
    bool muted = false, micMuted = false, dnd = false, wifiOn = false, btOn = false, dark = false, energy = false, night = false;
    bool hotspotAvail = false, hotspotOn = false, savedWifi = true, savedBt = true, btAsked = false;
    int bright = -1;
    svc::Battery battery;
    svc::PowerMode mode = svc::PowerMode::Unknown;
    std::wstring ssid;
    std::vector<Item> items;
    Item hover, pressed;
    bool tracking = false;
    bool powerOpen = false;
    WORD lastMin = 0xFFFF;
    Id drag = Id::None;
    ULONGLONG lastBrightSet = 0;
    WORD chordKey = 0;
    bool chordShift = false;


    Impl(ControlCenter* a, HINSTANCE i, HWND o, wuc::Compositor c, wuc::CompositionGraphicsDevice g)
        : api(a), inst(i), owner(o), comp(std::move(c)), gdev(std::move(g)) {}

    void RegisterClasses();
    void Open();
    void Close();
    void Teardown();
    void Focus();
    void BuildFonts();
    void SyncAll();
    void SyncSpeaker();
    void SyncMic();
    void SyncRadios();
    void StartLate();
    void Layout();
    void ApplySize();
    void Render();
    void Paint(ID2D1DeviceContext* dc, float tx, float ty, Item const* only, Item const* skip);
    void BuildVisuals();
    wuc::CompositionBrush NoiseBrush(int w, int h);
    void Pulse(Item const& it);
    void SetHover(Item const* it);
    void BeginPress(Item const& it);
    void EndPress();
    void Click(Item const& it);
    void LaunchAfterClose(wchar_t const* uri);
    void DeferChord(WORD key, bool shift);
    void DragTo(float x);
    void RunPowerAfterClose(int item);
    Item const* HitAt(float x, float y) const;
    float TextWidth(std::wstring const& t, IDWriteTextFormat* f);
    SIZE WindowPx() const;

    LRESULT PanelProc(HWND h, UINT m, WPARAM w, LPARAM l);
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
    RegisterClassExW(&wc);
    hot = CreateWindowExW(0, kHotClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, this);
}

// A message-only window that hosts the delayed Win-chord timer (Cast, Snip): it outlives the panel.
// Edge gestures that open the panel live in edge.cpp.
LRESULT CALLBACK ControlCenter::Impl::HotThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && m == WM_TIMER && w == kTimerChord) {
        KillTimer(h, kTimerChord);
        svc::SendWinChord(self->chordKey, self->chordShift);
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
    // Panel coordinates, logical: the window has motion::kPad of empty margin on every side.
    const float x = static_cast<short>(LOWORD(l)) / s - motion::kPad, y = static_cast<short>(HIWORD(l)) / s - motion::kPad;
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && st == St::Open) {  // clicked outside, unless it was the notification column
                HWND other = reinterpret_cast<HWND>(l);
                if (other && api->siblingHwnd && other == api->siblingHwnd()) return 0;
                svc::Log(L"panel deactivated, closing (focus went to " + std::to_wstring(reinterpret_cast<uintptr_t>(other)) + L")");
                Close();
            }
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) Close();
            return 0;
        case WM_LBUTTONDOWN:
            if (st != St::Open) return 0;
            if (x < 0 || y < 0 || x >= kW || y >= height) { Close(); return 0; }  // the transparent margin is "outside"
            if (auto* it = HitAt(x, y)) {
                if (it->id == Id::Bright || it->id == Id::Vol) {
                    drag = it->id;
                    SetCapture(hwnd);
                    DragTo(x);
                } else {
                    pressed = *it;
                    if (Scalable(it->id)) BeginPress(*it);
                }
            }
            return 0;
        case WM_MOUSEMOVE: {
            if (st != St::Open) return 0;
            if (drag != Id::None) { DragTo(x); return 0; }
            if (!tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                tracking = TrackMouseEvent(&tme);
            }
            auto* it = HitAt(x, y);
            Item now = it ? *it : Item{};
            if (!Same(now, hover)) { hover = now; SetHover(it); }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            if (hover.id != Id::None) { hover = {}; SetHover(nullptr); }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (drag != Id::None) {
                if (drag == Id::Bright && bright >= 0) svc::brightness::Set(bright);  // the final value always lands
                drag = Id::None;
                if (m == WM_LBUTTONUP) ReleaseCapture();
            } else if (pressed.id != Id::None) {
                Item p = std::exchange(pressed, Item{});
                auto* it = (m == WM_LBUTTONUP && st == St::Open) ? HitAt(x, y) : nullptr;
                if (it && Same(*it, p)) {
                    Item c = *it;  // Click re-lays-out and invalidates `it`
                    Click(c);
                } else if (Scalable(p.id) && st == St::Open) {
                    Render();
                }
                EndPress();
            }
            return 0;
        case kMsgAudio: SyncSpeaker(); Render(); return 0;
        case kMsgMic: SyncMic(); Render(); return 0;
        case kMsgRadios:
            SyncRadios();
            if (btOn && !btAsked) { btAsked = true; svc::BtRefresh(hwnd, kMsgBt); }  // for "connected device" under Bluetooth
            Render();
            return 0;
        case kMsgBright:
            if (svc::brightness::Supported()) bright = svc::brightness::Level();
            Render();
            return 0;
        case kMsgMedia: media.Pull(); return 0;
        case kMsgWifi: case kMsgBt: Render(); return 0;
        case kMsgLate: {
            std::unique_ptr<Late> r(reinterpret_cast<Late*>(l));
            if (st == St::Open && r) {
                hotspotAvail = r->hotspotAvail;
                hotspotOn = r->hotspotOn;
                ssid = r->ssid;
                Render();
            }
            return 0;
        }
        case WM_TIMER:
            if (w == kTimerClose) Teardown();
            else if (w == kTimerClock && st == St::Open) {
                SYSTEMTIME t;
                GetLocalTime(&t);
                if (t.wMinute != lastMin) Render();
            }
            return 0;
        case WM_NCDESTROY: hwnd = nullptr; break;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------------------------
// open / close

SIZE ControlCenter::Impl::WindowPx() const {
    int pad = static_cast<int>(std::lround(motion::kPad * s));
    return {static_cast<LONG>(std::ceil(kW * s)) + 2 * pad, static_cast<LONG>(std::ceil(height * s)) + 2 * pad};
}

void ControlCenter::Impl::Open() {
    if (st == St::Open) return;
    if (st == St::Closing) Teardown();
    LARGE_INTEGER q0, qf;
    QueryPerformanceCounter(&q0);
    QueryPerformanceFrequency(&qf);

    HMONITOR hm = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);  // one column on the primary monitor
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    mon = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s = dx / 96.0f;

    try {
        BuildFonts();  // first open only
        hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass,
                               L"Halo Control Center", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
        if (!hwnd) return;
        BOOL on = TRUE;
        DwmSetWindowAttribute(hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));
        SyncAll();
        Layout();
        const int pad = static_cast<int>(std::lround(motion::kPad * s)), mg = static_cast<int>(std::lround(kMargin * s));
        int wPx = static_cast<int>(std::ceil(kW * s)), hPx = static_cast<int>(std::ceil(height * s));
        rect = {mon.right - wPx - mg, mon.top + mg, 0, 0};
        rect.right = rect.left + wPx;
        rect.bottom = rect.top + hPx;
        SIZE wp = WindowPx();
        SetWindowPos(hwnd, HWND_TOPMOST, rect.left - pad, rect.top - pad, wp.cx, wp.cy, SWP_NOACTIVATE);
        BuildVisuals();
        st = St::Open;
        Render();
        pill.Opacity(0);
        ShowWindow(hwnd, SW_SHOW);
        motion::In(comp, pill, {pad * 1.0f, pad * 1.0f, 0}, {static_cast<float>(wPx), 0}, {0, -28.0f * s});
        {
            LARGE_INTEGER q1;
            QueryPerformanceCounter(&q1);
            svc::Log(L"open latency Open()->ShowWindow: " + std::to_wstring((q1.QuadPart - q0.QuadPart) * 10000 / qf.QuadPart / 10.0) + L" ms");
        }
        Focus();
        SetTimer(hwnd, kTimerClock, 15000, nullptr);
        // Async sources fill in as they arrive.
        radios.Start(hwnd, kMsgRadios);
        media.Start(hwnd, kMsgMedia, [this] { if (st == St::Open) Render(); });
        svc::brightness::Probe(hwnd, kMsgBright);
        StartLate();
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
    SetHover(nullptr);
    hover = pressed = {};
    drag = Id::None;
    const float pad = motion::kPad * s;
    motion::Out(comp, pill, {pad, pad, 0}, {std::ceil(kW * s), 0}, {0, -28.0f * s});
    SetTimer(hwnd, kTimerClose, motion::kOutMs + 10, nullptr);
    if (api->onHidden) api->onHidden();
}

// Releases the window and every composition object (the DirectWrite objects and grain stay cached).
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
    powerOpen = false;
    expand = Expand::None;
    btAsked = false;
    hover = pressed = {};
    tracking = false;
    hvShown = false;
    if (hwnd) { KillTimer(hwnd, kTimerClose); DestroyWindow(hwnd); }
    hwnd = nullptr;
    if (surf) surf.Close();
    if (pvSurf) pvSurf.Close();
    surf = nullptr; pvSurf = nullptr; content = nullptr; pv = nullptr; hv = nullptr; hvGeo = nullptr;
    pill = nullptr; fx = nullptr; root = nullptr; target = nullptr; clip = nullptr; rim = nullptr;
    items.clear();
    st = St::Closed;
    rect = {};
    if (wasOpen && api->onHidden) api->onHidden();
}

// A soft ring that springs out of a toggle's icon: the "springy" feedback for a click. One
// compositor animation, no timer; the next pulse replaces it.
void ControlCenter::Impl::Pulse(Item const& it) {
    if (!fx) return;
    fx.Children().RemoveAll();
    float rad = std::min(it.r.right - it.r.left, it.r.bottom - it.r.top) / 2 - 2;
    auto geo = comp.CreateEllipseGeometry();
    geo.Radius({rad * s, rad * s});
    auto shape = comp.CreateSpriteShape(geo);
    shape.StrokeBrush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0xB0, 0xFF, 0xFF, 0xFF)));
    shape.StrokeThickness(2 * s);
    auto v = comp.CreateShapeVisual();
    v.Size({1, 1});
    v.Offset({(it.r.left + it.r.right) / 2 * s, (it.r.top + it.r.bottom) / 2 * s, 0});
    v.Shapes().Append(shape);
    fx.Children().InsertAtTop(v);
    auto sc = comp.CreateVector3KeyFrameAnimation();
    sc.InsertKeyFrame(0.0f, {0.9f, 0.9f, 1.0f});
    sc.InsertKeyFrame(1.0f, {1.5f, 1.5f, 1.0f}, comp.CreateCubicBezierEasingFunction({0.2f, 0.8f}, {0.3f, 1.0f}));
    sc.Duration(380ms);
    v.StartAnimation(L"Scale", sc);
    auto op = comp.CreateScalarKeyFrameAnimation();
    op.InsertKeyFrame(0.0f, 1.0f);
    op.InsertKeyFrame(1.0f, 0.0f);
    op.Duration(380ms);
    v.StartAnimation(L"Opacity", op);
}

// One composition highlight that glides between controls (the surface itself is not redrawn on hover).
void ControlCenter::Impl::SetHover(Item const* it) {
    if (!hv || !hvGeo) return;
    bool show = it && it->id != Id::Bright && it->id != Id::Vol && !(pressed.id != Id::None && Scalable(pressed.id));
    if (!show) {
        if (hvShown) {
            hvShown = false;
            auto a = comp.CreateScalarKeyFrameAnimation();
            a.InsertKeyFrame(1.0f, 0.0f);
            a.Duration(140ms);
            hv.StartAnimation(L"Opacity", a);
        }
        return;
    }
    hvGeo.CornerRadius({it->rad * s, it->rad * s});
    wfn::float2 off{it->r.left * s, it->r.top * s}, sz{(it->r.right - it->r.left) * s, (it->r.bottom - it->r.top) * s};
    if (hvShown) {
        auto ease = comp.CreateCubicBezierEasingFunction({0.2f, 0.0f}, {0.0f, 1.0f});
        auto o = comp.CreateVector2KeyFrameAnimation();
        o.InsertKeyFrame(1.0f, off, ease);
        o.Duration(110ms);
        hvGeo.StartAnimation(L"Offset", o);
        auto z = comp.CreateVector2KeyFrameAnimation();
        z.InsertKeyFrame(1.0f, sz, ease);
        z.Duration(110ms);
        hvGeo.StartAnimation(L"Size", z);
    } else {
        hvGeo.StopAnimation(L"Offset");
        hvGeo.StopAnimation(L"Size");
        hvGeo.Offset(off);
        hvGeo.Size(sz);
        hvShown = true;
    }
    auto a = comp.CreateScalarKeyFrameAnimation();
    a.InsertKeyFrame(1.0f, 1.0f);
    a.Duration(110ms);
    hv.StartAnimation(L"Opacity", a);
}

// Press feedback: the control's own drawing is lifted onto a small surface and springs to 0.96 while
// the base surface skips it (so nothing is drawn twice). One re-render, one animation.
void ControlCenter::Impl::BeginPress(Item const& it) {
    if (!pill || st != St::Open) return;
    if (pv) {
        pill.Children().Remove(pv);
        pv = nullptr;
        if (pvSurf) { pvSurf.Close(); pvSurf = nullptr; }
    }
    SetHover(nullptr);
    Render();  // `pressed` is set: the base surface leaves this control out
    int l = static_cast<int>(std::floor(it.r.left * s)), t = static_cast<int>(std::floor(it.r.top * s));
    int w = static_cast<int>(std::ceil(it.r.right * s)) - l, h = static_cast<int>(std::ceil(it.r.bottom * s)) - t;
    pvSurf = gdev.CreateDrawingSurface({static_cast<float>(w), static_cast<float>(h)},
                                       winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                       winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    auto interop = pvSurf.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT off{};
    if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return;
    try {
        Paint(dc.get(), static_cast<float>(off.x - l), static_cast<float>(off.y - t), &it, nullptr);
    } catch (...) {}
    interop->EndDraw();
    pv = comp.CreateSpriteVisual();
    pv.Size({static_cast<float>(w), static_cast<float>(h)});
    pv.Offset({static_cast<float>(l), static_cast<float>(t), 0});
    pv.CenterPoint({w / 2.0f, h / 2.0f, 0});
    auto sb = comp.CreateSurfaceBrush(pvSurf);
    sb.Stretch(wuc::CompositionStretch::None);
    pv.Brush(sb);
    pill.Children().InsertAtTop(pv);
    auto spring = comp.CreateSpringVector3Animation();
    spring.InitialValue(Ref3(1.0f));
    spring.FinalValue(Ref3(0.96f));
    spring.DampingRatio(0.55f);
    spring.Period(40ms);
    pv.StartAnimation(L"Scale", spring);
}

void ControlCenter::Impl::EndPress() {
    if (!pv) return;
    auto spring = comp.CreateSpringVector3Animation();
    spring.FinalValue(Ref3(1.0f));
    spring.DampingRatio(0.5f);
    spring.Period(45ms);
    pv.StartAnimation(L"Scale", spring);
    auto fade = comp.CreateScalarKeyFrameAnimation();  // the base surface has the control again: cross-fade
    fade.InsertKeyFrame(1.0f, 0.0f);
    fade.Duration(160ms);
    pv.StartAnimation(L"Opacity", fade);
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
    ssid = wifiOn ? svc::ConnectedSsid() : std::wstring();
}

// Only fast reads here: the hotspot and SSID queries (20+ ms between them) come in through StartLate.
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
    night = svc::ReadNightLight();
    bright = svc::brightness::Supported() ? svc::brightness::Level() : -1;
}

void ControlCenter::Impl::StartLate() {
    HWND h = hwnd;
    std::thread([h] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto* r = new Late;
        r->hotspotAvail = svc::HotspotAvailable();
        r->hotspotOn = r->hotspotAvail && svc::ReadHotspot();
        r->ssid = svc::ConnectedSsid();
        if (!PostMessageW(h, kMsgLate, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

// The shell's own chords go out after the panel has faded, so it isn't in the snip.
void ControlCenter::Impl::DeferChord(WORD key, bool shift) {
    chordKey = key;
    chordShift = shift;
    Close();
    if (hot) SetTimer(hot, kTimerChord, 280, nullptr);
    else svc::SendWinChord(key, shift);
}

// Launches a URI once the panel has faded, so it isn't in the capture / doesn't steal the focus back.
void ControlCenter::Impl::LaunchAfterClose(wchar_t const* uri) {
    Close();
    std::wstring u = uri;
    std::thread([u] {
        Sleep(280);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        svc::OpenSettings(u.c_str());
        CoUninitialize();
    }).detach();
}

// Sleep / Restart / Shut down from the inline choice. The panel fades first; under HALO_TEST or
// HALO_TEST_POWER_DRYRUN (or the CC dry run) it only logs.
void ControlCenter::Impl::RunPowerAfterClose(int item) {
    static wchar_t const* const names[] = {L"Sleep", L"Restart", L"Shut down"};
    wchar_t const* env = _wgetenv(L"HALO_TEST_POWER_DRYRUN");
    if (svc::DryRun() || _wgetenv(L"HALO_TEST") || (env && env[0] == L'1')) {
        svc::Log(std::wstring(L"power: DRY RUN, would run ") + names[item]);
        Close();
        return;
    }
    svc::Log(std::wstring(L"power: ") + names[item]);
    Close();
    std::thread([item] {
        Sleep(280);
        constexpr DWORD kReason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_MINOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
        if (item == 0) { SetSuspendState(FALSE, FALSE, FALSE); return; }
        HANDLE t = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) {
            TOKEN_PRIVILEGES tp{};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid)) AdjustTokenPrivileges(t, FALSE, &tp, 0, nullptr, nullptr);
            CloseHandle(t);
        }
        ExitWindowsEx(item == 1 ? EWX_REBOOT : EWX_POWEROFF, kReason);
    }).detach();
}

void ControlCenter::Impl::Click(Item const& it) {
    if (svc::DryRun()) svc::Log(L"click id=" + std::to_wstring(static_cast<int>(it.id)) + L" arg=" + std::to_wstring(it.arg));
    switch (it.id) {
        case Id::Battery:
            if (mode == svc::PowerMode::Unknown) return;
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
        case Id::WifiText:
            expand = expand == Expand::Wifi ? Expand::None : Expand::Wifi;
            if (expand == Expand::Wifi) svc::WifiRefresh(hwnd, kMsgWifi);
            break;
        case Id::BtText:
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
            if (!svc::DryRun()) dark = svc::ReadDarkMode();
            Pulse(it);
            break;
        case Id::Night:
            night = !night;
            svc::SetNightLight(night);
            if (!svc::DryRun()) night = svc::ReadNightLight();
            Pulse(it);
            break;
        case Id::Snip: LaunchAfterClose(L"ms-screenclip:"); return;
        case Id::Cast: LaunchAfterClose(L"ms-settings-connectabledevices:devicediscovery"); return;
        case Id::Settings: LaunchAfterClose(L"ms-settings:"); return;
        case Id::Lock:
            if (svc::DryRun()) { svc::Log(L"DRYRUN lock workstation"); break; }
            Close();
            std::thread([] { Sleep(250); LockWorkStation(); }).detach();
            return;
        case Id::Power: powerOpen = !powerOpen; break;
        case Id::PowSleep: RunPowerAfterClose(0); return;
        case Id::PowRestart: RunPowerAfterClose(1); return;
        case Id::PowShut: RunPowerAfterClose(2); return;
        case Id::Mic:
            if (!mic.Available()) return;
            micMuted = !micMuted;
            mic.SetMute(micMuted);
            Pulse(it);
            break;
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
    const float x0 = kInset, x1 = kW - kInset;
    float y = kInset;
    auto add = [&](Id id, D2D1_RECT_F r, int arg = 0, float rad = 10) { items.push_back({id, r, arg, rad}); };

    // Header module: the time and date, and the power mode + battery (click cycles the mode).
    add(Id::Header, {x0, y, x1, y + kHeadH}, 0, kTileR);
    add(Id::Battery, {x1 - 14 - 124, y + (kHeadH - 44) / 2, x1 - 14, y + (kHeadH - 44) / 2 + 44}, 0, 16);
    y += kHeadH + kGap;

    // Row 1: two half-width pills. The icon toggles the radio, the text (and chevron) opens its list.
    const float pw = (x1 - x0 - kGap) / 2;
    for (int i = 0; i < 2; i++) {
        bool wifi = i == 0;
        float l = x0 + i * (pw + kGap);
        D2D1_RECT_F r{l, y, l + pw, y + kPillH};
        add(wifi ? Id::WifiPill : Id::BtPill, r, 0, kTileR);
        add(wifi ? Id::WifiText : Id::BtText, r, 0, kTileR);
        float cx = l + 30, cy = y + kPillH / 2;
        add(wifi ? Id::Wifi : Id::Bluetooth, {cx - 20, cy - 20, cx + 20, cy + 20}, 0, 20);
    }
    y += kPillH + kGap;

    // The inline list under the pills.
    if (expand != Expand::None) {
        bool wifi = expand == Expand::Wifi;
        size_t n = wifi ? svc::WifiNets().size() : svc::BtDevices().size();
        float h = 40 + std::max<size_t>(1, n) * 32 + 6;
        add(Id::ListTile, {x0, y, x1, y + h}, 0, kTileR);
        for (size_t i = 0; i < n; i++)
            add(wifi ? Id::WifiNet : Id::BtDev, {x0 + 8, y + 40 + i * 32, x1 - 8, y + 72 + i * 32}, static_cast<int>(i), 12);
        y += h + kGap;
    }

    // Row 2: round icon-only buttons.
    {
        add(Id::RowTile, {x0, y, x1, y + kRowH}, 0, kTileR);
        Id ids[] = {Id::Airplane, Id::Hotspot, Id::Night, Id::Dark, Id::Dnd};
        float step = (x1 - x0 - 64) / 4;
        for (int i = 0; i < 5; i++) {
            float cx = x0 + 32 + i * step, cy = y + kRowH / 2;
            add(ids[i], {cx - 22, cy - 22, cx + 22, cy + 22}, 0, 22);
        }
        y += kRowH + kGap;
    }

    // Slim Display and Sound sliders.
    if (bright >= 0) {
        add(Id::DisplayTile, {x0, y, x1, y + kSliderH}, 0, kTileR);
        add(Id::Bright, {x0 + 14, y + 27, x1 - 14, y + 47});
        y += kSliderH + kGap;
    }
    if (speaker.Available()) {
        float extra = outsOpen ? outs.size() * 32.0f + 6 : 0;
        add(Id::SoundTile, {x0, y, x1, y + kSliderH + extra}, 0, kTileR);
        add(Id::OutRow, {x0 + 100, y + 4, x1 - 8, y + 26}, 0, 10);
        add(Id::Vol, {x0 + 14, y + 27, x1 - 14, y + 47});
        if (outsOpen)
            for (size_t i = 0; i < outs.size(); i++)
                add(Id::OutDev, {x0 + 8, y + kSliderH - 2 + i * 32, x1 - 8, y + kSliderH + 30 + i * 32}, static_cast<int>(i), 12);
        y += kSliderH + extra + kGap;
    }
    if (media.State().has) {
        add(Id::MediaTile, {x0, y, x1, y + kMediaH}, 0, kTileR);
        add(Id::Prev, {x0 + 84, y + 46, x0 + 120, y + 80}, 0, 12);
        add(Id::PlayPause, {x0 + 120, y + 46, x0 + 156, y + 80}, 0, 12);
        add(Id::Next, {x0 + 156, y + 46, x0 + 192, y + 80}, 0, 12);
        y += kMediaH + kGap;
    }

    // Quick actions: round buttons.
    {
        add(Id::RowTile, {x0, y, x1, y + kActH}, 0, kTileR);
        Id ids[] = {Id::Settings, Id::Lock, Id::Power, Id::Mic, Id::Snip, Id::Cast};
        float step = (x1 - x0 - 2 * 30) / 5;
        for (int i = 0; i < 6; i++) {
            float cx = x0 + 30 + i * step, cy = y + kActH / 2;
            add(ids[i], {cx - 22, cy - 22, cx + 22, cy + 22}, 0, 22);
        }
        y += kActH + kGap;
    }
    // The inline Sleep / Restart / Shut down choice under the actions.
    if (powerOpen) {
        add(Id::PowerTile, {x0, y, x1, y + kPowH}, 0, kTileR);
        Id ids[] = {Id::PowSleep, Id::PowRestart, Id::PowShut};
        float w3 = (x1 - x0 - 16) / 3;
        for (int i = 0; i < 3; i++) add(ids[i], {x0 + 8 + i * w3, y + 6, x0 + 8 + (i + 1) * w3, y + kPowH - 6}, 0, 14);
        y += kPowH + kGap;
    }
    y += kInset - kGap;
    height = std::min({y, kMaxH, std::max(200.0f, (mon.bottom - mon.top) / s - 2 * kMargin)});
}

// Window, visuals and surface all follow `height`.
void ControlCenter::Impl::ApplySize() {
    int wPx = static_cast<int>(std::ceil(kW * s)), hPx = static_cast<int>(std::ceil(height * s));
    if (rect.bottom - rect.top == hPx) return;
    rect.bottom = rect.top + hPx;
    SIZE wp = WindowPx();
    SetWindowPos(hwnd, nullptr, 0, 0, wp.cx, wp.cy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pill.Size({static_cast<float>(wPx), static_cast<float>(hPx)});
    content.Size({static_cast<float>(wPx), static_cast<float>(hPx)});
    clip.Size({static_cast<float>(wPx), static_cast<float>(hPx)});
    rim.Size({wPx - s, hPx - s});
    surf.as<ICompositionDrawingSurfaceInterop>()->Resize({wPx, hPx});
    if (st == St::Open && api->onResized) api->onResized();
}

void ControlCenter::Impl::Render() {
    if (!surf || st != St::Open) return;
    Layout();
    ApplySize();
    auto interop = surf.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT off{};
    if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return;
    try {
        Item const* skip = (pressed.id != Id::None && Scalable(pressed.id)) ? &pressed : nullptr;
        Paint(dc.get(), static_cast<float>(off.x), static_cast<float>(off.y), nullptr, skip);
    } catch (...) {}
    interop->EndDraw();
}

// Draws every item (or just `only`, minus `skip`) with the panel origin at (tx, ty) device px.
void ControlCenter::Impl::Paint(ID2D1DeviceContext* dc, float tx, float ty, Item const* only, Item const* skip) {
    dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(tx, ty));
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
    // A round control: accent when on, glass when off; a glyph, or a custom mark for dark mode.
    auto icon = [&](float cx, float cy, float rad, bool on, bool avail, wchar_t g, IDWriteTextFormat* f, bool darkMark) {
        float a = avail ? 1.0f : 0.45f;
        circle(cx, cy, rad, on ? ColorF(0x0A84FF, a) : ColorF(1, 1, 1, 0.17f * a));
        if (darkMark) {  // half-filled disc
            float k = rad * 0.34f;
            br->SetColor(ColorF(1, 1, 1, a));
            dc->DrawEllipse(D2D1::Ellipse({cx, cy}, k, k), br.get(), 1.6f);
            winrt::com_ptr<ID2D1Factory> fac;
            dc->GetFactory(fac.put());
            winrt::com_ptr<ID2D1PathGeometry> pg;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(fac->CreatePathGeometry(pg.put())) && SUCCEEDED(pg->Open(sink.put()))) {
                sink->BeginFigure({cx, cy - k}, D2D1_FIGURE_BEGIN_FILLED);
                sink->AddArc(D2D1::ArcSegment({cx, cy + k}, {k, k}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                dc->FillGeometry(pg.get(), br.get());
            }
        } else {
            glyph(g, f, {cx - rad, cy - rad, cx + rad, cy + rad}, ColorF(1, 1, 1, a));
        }
    };

    for (auto const& it : items) {
        if (only && !Same(it, *only)) continue;
        if (skip && Same(it, *skip)) continue;
        auto const& r = it.r;
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        switch (it.id) {
            case Id::WifiPill: case Id::BtPill: case Id::RowTile: case Id::DisplayTile: case Id::SoundTile: case Id::ListTile: case Id::MediaTile: case Id::Header: case Id::PowerTile:
                tile(r, kTileR);
                if (it.id == Id::Header) {
                    wchar_t tb[32] = L"", db[64] = L"";
                    SYSTEMTIME now;
                    GetLocalTime(&now);
                    lastMin = now.wMinute;
                    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS | TIME_NOTIMEMARKER, &now, nullptr, tb, 32);
                    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"dddd, d MMMM", db, 64, nullptr);
                    txt(tb, fTime.get(), {r.left + 16, r.top + 6, r.left + 170, r.top + 52}, kText);
                    txt(db, fDate.get(), {r.left + 17, r.top + 50, r.left + 176, r.top + 70}, kDim);
                }
                if (it.id == Id::DisplayTile) txt(L"Display", fTitle.get(), {r.left + 16, r.top + 4, r.right - 16, r.top + 25}, kText);
                if (it.id == Id::SoundTile) txt(L"Sound", fTitle.get(), {r.left + 16, r.top + 4, r.left + 96, r.top + 25}, kText);
                if (it.id == Id::ListTile)
                    txt(expand == Expand::Wifi ? L"Known Wi-Fi networks" : L"Paired Bluetooth devices", fTitle.get(),
                        {r.left + 16, r.top + 10, r.right - 16, r.top + 32}, kText);
                break;
            case Id::WifiText: case Id::BtText: {
                bool wifi = it.id == Id::WifiText;
                bool avail = wifi ? radios.HasWifi() : radios.HasBluetooth(), on = wifi ? wifiOn : btOn;
                std::wstring sub;
                if (wifi) sub = !avail ? L"Unavailable" : !on ? L"Off" : ssid.empty() ? L"Not connected" : ssid;
                else {
                    sub = !avail ? L"Unavailable" : on ? L"On" : L"Off";
                    if (on) for (auto const& d : svc::BtDevices()) if (d.connected) { sub = d.name; break; }
                }
                float a = avail ? 1.0f : 0.45f, tx = r.left + 54, rt = r.right - 24;
                txt(wifi ? L"Wi-Fi" : L"Bluetooth", fTitle.get(), {tx, cy - 18, rt, cy}, ColorF(1, 1, 1, 0.95f * a));
                txt(sub, fSub.get(), {tx, cy + 1, rt, cy + 17}, ColorF(1, 1, 1, 0.58f * a));
                bool open = expand == (wifi ? Expand::Wifi : Expand::Bluetooth);
                glyph(open ? 0xE70E : 0xE76C, fIconS.get(), {r.right - 26, cy - 10, r.right - 8, cy + 10}, kDim);  // chevron up / right
                break;
            }
            case Id::Wifi: icon(cx, cy, 18, wifiOn, radios.HasWifi(), 0xE701, fIconS.get(), false); break;
            case Id::Bluetooth: icon(cx, cy, 18, btOn, radios.HasBluetooth(), 0xE702, fIconS.get(), false); break;
            case Id::Airplane: {
                bool avail = radios.HasWifi() || radios.HasBluetooth();
                icon(cx, cy, 22, avail && !wifiOn && !btOn, avail, 0xE709, fIcon.get(), false);
                break;
            }
            case Id::Hotspot: icon(cx, cy, 22, hotspotOn, hotspotAvail, 0xE704, fIcon.get(), false); break;
            case Id::Night: icon(cx, cy, 22, night, true, 0xF08C, fIcon.get(), false); break;
            case Id::Dark: icon(cx, cy, 22, dark, true, 0, fIcon.get(), true); break;
            case Id::Dnd: icon(cx, cy, 22, dnd, true, 0xE708, fIcon.get(), false); break;
            case Id::Snip: icon(cx, cy, 22, false, true, 0xE8C6, fIcon.get(), false); break;
            case Id::Cast: icon(cx, cy, 22, false, true, 0xE7F4, fIcon.get(), false); break;
            case Id::Settings: icon(cx, cy, 22, false, true, 0xE713, fIcon.get(), false); break;
            case Id::Lock: icon(cx, cy, 22, false, true, 0xE72E, fIcon.get(), false); break;
            case Id::Power: icon(cx, cy, 22, powerOpen, true, 0xE7E8, fIcon.get(), false); break;
            case Id::Mic: icon(cx, cy, 22, micMuted, mic.Available(), micMuted ? 0xEC54 : 0xE720, fIcon.get(), false); break;
            case Id::PowSleep: case Id::PowRestart: case Id::PowShut:
                box(r, 14, ColorF(1, 1, 1, 0.10f));
                txt(it.id == Id::PowSleep ? L"Sleep" : it.id == Id::PowRestart ? L"Restart" : L"Shut down", fBtn.get(), r, kText);
                break;
            case Id::Battery: {
                box(r, 16, ColorF(1, 1, 1, 0.12f));
                if (battery.hasBattery) {
                    int lvl = std::min(10, battery.percent / 10);
                    wchar_t g = static_cast<wchar_t>((battery.ac ? 0xEBAB : 0xEBA0) + lvl);
                    glyph(g, fIcon.get(), {r.left + 6, r.top, r.left + 32, r.bottom}, kText);
                } else {
                    glyph(0xE83E, fIcon.get(), {r.left + 6, r.top, r.left + 32, r.bottom}, kText);  // plug
                }
                std::wstring cap = battery.ac ? L"Plugged in" : L"Battery";
                switch (mode) {
                    case svc::PowerMode::Efficiency: cap = L"Efficiency"; break;
                    case svc::PowerMode::Balanced: cap = L"Balanced"; break;
                    case svc::PowerMode::Performance: case svc::PowerMode::Max: cap = L"Performance"; break;
                    default: break;
                }
                std::wstring top = battery.hasBattery ? std::to_wstring(battery.percent) + L"%" : cap;
                txt(top, fBatt.get(), {r.left + 36, r.top + 4, r.right - 6, cy + 2}, kText);
                if (battery.hasBattery) txt(cap, fSub.get(), {r.left + 36, cy + 1, r.right - 6, r.bottom - 3}, kDim);
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
                txt(name, fSubR.get(), {r.left + 4, r.top, r.right - 26, r.bottom}, kDim);
                glyph(outsOpen ? 0xE70E : 0xE70D, fIconS.get(), {r.right - 26, r.top, r.right - 4, r.bottom}, kDim);
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
                if (mark) box(r, 12, ColorF(1, 1, 1, 0.07f));
                txt(name, fSub.get(), {r.left + 12, r.top, r.right - (sub.empty() ? 36 : 112), r.bottom}, mark ? kText : kDim);
                if (!sub.empty()) txt(sub, fSubR.get(), {r.right - 104, r.top, r.right - 36, r.bottom}, kDim);
                if (mark) glyph(0xE73E, fIconS.get(), {r.right - 32, r.top, r.right - 8, r.bottom}, kAccent);
                break;
            }
            case Id::Prev: case Id::PlayPause: case Id::Next: {
                auto const& ms = media.State();
                bool ok = it.id == Id::Prev ? ms.canPrev : it.id == Id::Next ? ms.canNext : true;
                wchar_t g = it.id == Id::Prev ? 0xE892 : it.id == Id::Next ? 0xE893 : (ms.playing ? 0xE769 : 0xE768);
                glyph(g, fIcon.get(), r, ColorF(1, 1, 1, ok ? 0.95f : 0.3f));
                break;
            }
            default: break;
        }
        if (it.id == Id::MediaTile) {
            auto const& ms = media.State();
            D2D1_RECT_F art{r.left + 14, r.top + 14, r.left + 74, r.top + 74};
            if (ms.artSize > 0 && ms.art.size() == static_cast<size_t>(ms.artSize) * ms.artSize) {
                winrt::com_ptr<ID2D1Bitmap> bmp;
                auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                winrt::com_ptr<ID2D1BitmapBrush> bb;
                if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(ms.artSize, ms.artSize), ms.art.data(), ms.artSize * 4, &props, bmp.put())) &&
                    SUCCEEDED(dc->CreateBitmapBrush(bmp.get(), bb.put()))) {
                    float k = 60.0f / ms.artSize;
                    bb->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(art.left, art.top));
                    bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(art, 12, 12), bb.get());
                }
            } else {
                box(art, 12, kOff);
                glyph(0xE8D6, fIcon.get(), art, kDim);  // music note
            }
            txt(ms.title.empty() ? L"Now Playing" : ms.title, fTitle.get(), {r.left + 88, r.top + 14, r.right - 16, r.top + 32}, kText);
            txt(ms.artist, fSub.get(), {r.left + 88, r.top + 30, r.right - 16, r.top + 48}, kDim);
        }
    }
}

// ---------------------------------------------------------------------------------------------
// building blocks

// Created once and kept: the factory and formats are in logical px (the DPI scale is a transform), so
// they stay valid across opens and monitors.
void ControlCenter::Impl::BuildFonts() {
    if (dw) return;
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
    mk(fTitle, L"Segoe UI Variable Text", SB, 13, L, M);
    mk(fSub, L"Segoe UI Variable Text", R, 11.5f, L, M);
    mk(fSubR, L"Segoe UI Variable Text", R, 11.5f, T, M);
    mk(fBatt, L"Segoe UI Variable Text", SB, 14, L, M);
    mk(fIcon, L"Segoe Fluent Icons", R, 18, C, M);
    mk(fIconS, L"Segoe Fluent Icons", R, 14, C, M);
    mk(fCap, L"Segoe UI Variable Text", R, 11, C, N);
    mk(fTime, L"Segoe UI Variable Display", SB, 34, L, N);
    mk(fDate, L"Segoe UI Variable Text", R, 12, L, N);
    mk(fBtn, L"Segoe UI Variable Text", SB, 12.5f, C, M);
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
    const float wPx = std::ceil(kW * s), hPx = std::ceil(height * s), pad = std::lround(motion::kPad * s);
    winrt::check_hresult(comp.as<ICompositorDesktopInterop>()->CreateDesktopWindowTarget(hwnd, TRUE, winrt::put_abi(target)));
    root = comp.CreateContainerVisual();
    root.RelativeSizeAdjustment({1, 1});
    target.Root(root);
    pill = comp.CreateContainerVisual();
    pill.Size({wPx, hPx});
    pill.Offset({pad, pad, 0});
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
        if (!noiseBrush || noiseW != static_cast<int>(wPx)) {  // generated once per scale, then shared by every open
            noiseBrush = NoiseBrush(static_cast<int>(wPx), static_cast<int>(std::ceil(kMaxH * s)));
            noiseW = static_cast<int>(wPx);
        }
        noise.Brush(noiseBrush);
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

    // The hover highlight: one rounded rect that fades and glides between controls.
    hv = comp.CreateShapeVisual();
    hv.RelativeSizeAdjustment({1, 1});
    hv.Opacity(0);
    hvGeo = comp.CreateRoundedRectangleGeometry();
    auto hs = comp.CreateSpriteShape(hvGeo);
    hs.FillBrush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0x26, 0xFF, 0xFF, 0xFF)));
    hv.Shapes().Append(hs);
    pill.Children().InsertAtTop(hv);
    hvShown = false;

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
}

ControlCenter::~ControlCenter() {
    m->Teardown();
    if (m->hot) DestroyWindow(m->hot);
}

void ControlCenter::Toggle() { IsOpen() ? Close() : Open(); }
void ControlCenter::Open() { m->Open(); }
void ControlCenter::Close() { m->Close(); }
bool ControlCenter::IsOpen() const { return m->st == Impl::St::Open; }
// The panel itself (physical px), not the padded window.
RECT ControlCenter::PanelRectScreen() const { return m->st == Impl::St::Closed ? RECT{} : m->rect; }
HWND ControlCenter::Hwnd() const { return m->st == Impl::St::Closed ? nullptr : m->hwnd; }

}  // namespace halo
