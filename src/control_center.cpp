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
using namespace std::chrono_literals;
using D2D1::ColorF;

namespace halo {
namespace {

// Layout, in logical px (all drawing is scaled by the monitor DPI).
constexpr float kW = 336, kPad = 12, kGap = 10, kRadius = 20, kTileRadius = 14;
constexpr float kHeaderH = 62, kTileH = 58, kSliderH = 36, kOutRowH = 28, kOutDevH = 30, kMediaH = 66;
constexpr float kMaxH = 640;
constexpr float kMargin = 8;  // gap to the screen edges
constexpr int kMaxOutputs = 6;

constexpr UINT kMsgAudio = WM_APP + 1, kMsgMic = WM_APP + 2, kMsgRadios = WM_APP + 3, kMsgBright = WM_APP + 4,
               kMsgMedia = WM_APP + 5;
constexpr UINT_PTR kTimerClose = 1, kTimerHot = 1;
constexpr UINT kCloseMs = 130, kHotDwellMs = 180;
constexpr wchar_t kPanelClass[] = L"HaloControlCenter", kHotClass[] = L"HaloControlCenterHotZone";

enum class Id { None, Header, Power, Wifi, Bluetooth, Dnd, Mic, Bright, Vol, OutRow, OutDev, Media, Prev, PlayPause, Next };
struct Item {
    Id id;
    D2D1_RECT_F r;
    int arg = 0;
};

winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::Numerics::float3> Ref3(float v) {
    return winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::Numerics::float3>(
        winrt::Windows::Foundation::Numerics::float3{v, v, 1.0f});
}

bool In(D2D1_RECT_F const& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

const D2D1_COLOR_F kText = ColorF(1, 1, 1, 0.94f), kDim = ColorF(1, 1, 1, 0.58f), kTile = ColorF(1, 1, 1, 0.09f),
                   kOff = ColorF(1, 1, 1, 0.16f), kAccent = ColorF(0x0A84FF), kFill = ColorF(1, 1, 1, 0.92f),
                   kOnFill = ColorF(0x1C1C1E);

}  // namespace

struct ControlCenter::Impl {
    HINSTANCE inst;
    HWND owner;
    wuc::Compositor comp;
    wuc::CompositionGraphicsDevice gdev;

    // ---- window + composition (exist only while open) ----
    enum class St { Closed, Open, Closing } st = St::Closed;
    HWND hwnd{}, hot{};
    float s = 1;
    RECT mon{};
    float height = 0;  // logical
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr}, pill{nullptr};
    wuc::SpriteVisual content{nullptr};
    wuc::CompositionDrawingSurface surf{nullptr};
    wuc::CompositionRoundedRectangleGeometry clip{nullptr}, rim{nullptr};

    winrt::com_ptr<IDWriteFactory> dw;
    winrt::com_ptr<IDWriteTextFormat> fTime, fDate, fTitle, fSub, fSubR, fBatt, fIcon, fIconS, fPill;

    // ---- state ----
    svc::AudioEndpoint speaker, mic;
    svc::Radios radios;
    svc::Media media;
    std::vector<svc::Device> outs;
    bool outsOpen = false;
    float volume = 0;
    bool muted = false, micMuted = false, dnd = false, wifiOn = false, btOn = false;
    int bright = -1;
    svc::Battery battery;
    svc::PowerMode mode = svc::PowerMode::Unknown;
    std::wstring ssid;
    std::vector<Item> items;
    Id drag = Id::None;
    ULONGLONG lastBrightSet = 0;

    // ---- hot zone ----
    bool hotTracking = false;

    Impl(HINSTANCE i, HWND o, wuc::Compositor c, wuc::CompositionGraphicsDevice g)
        : inst(i), owner(o), comp(std::move(c)), gdev(std::move(g)) {}

    void RegisterClasses();
    void PlaceHotZone();
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
    void Click(Item const& it);
    void DragTo(float x);
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

// A 2 px wide strip on the right screen edge, just below the caption-button row, so it never
// covers the close button of a maximized window. Nearly invisible (alpha 1), non-activating.
void ControlCenter::Impl::PlaceHotZone() {
    HMONITOR m = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(m, &mi);
    UINT dpi = 96, dy = 96;
    GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dpi, &dy);
    float k = dpi / 96.0f;
    int w = std::max(2, static_cast<int>(2 * k)), y = mi.rcMonitor.top + static_cast<int>(36 * k), h = static_cast<int>(96 * k);
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

// Event-driven: the pointer entering the strip arms one dwell timer; leaving disarms it.
LRESULT ControlCenter::Impl::HotProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_MOUSEMOVE:
            if (!hotTracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                hotTracking = TrackMouseEvent(&tme);
                SetTimer(h, kTimerHot, kHotDwellMs, nullptr);
            }
            return 0;
        case WM_MOUSELEAVE:
            hotTracking = false;
            KillTimer(h, kTimerHot);
            return 0;
        case WM_TIMER:
            if (w == kTimerHot) {
                KillTimer(h, kTimerHot);
                if (st == St::Closed) Open();
            }
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK ControlCenter::Impl::PanelThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->PanelProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
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
            for (auto it = items.rbegin(); it != items.rend(); ++it) {
                if (it->id == Id::Header || it->id == Id::Media || !In(it->r, x, y)) continue;
                if (it->id == Id::Bright || it->id == Id::Vol) {
                    drag = it->id;
                    SetCapture(hwnd);
                    DragTo(x);
                } else {
                    Click(*it);
                }
                break;
            }
            return 0;
        case WM_MOUSEMOVE:
            if (drag != Id::None) DragTo(x);
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            if (drag != Id::None) {
                if (drag == Id::Bright && bright >= 0) svc::brightness::Set(bright);  // the final value always lands
                drag = Id::None;
                if (m == WM_LBUTTONUP) ReleaseCapture();
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
        SetWindowPos(hwnd, HWND_TOPMOST, mon.right - wPx - static_cast<int>(kMargin * s), mon.top + static_cast<int>(kMargin * s),
                     wPx, hPx, SWP_NOACTIVATE);
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
    Animate(false);
    SetTimer(hwnd, kTimerClose, kCloseMs, nullptr);
}

// Releases the window and every composition and DirectWrite object.
void ControlCenter::Impl::Teardown() {
    speaker.Unbind();
    mic.Unbind();
    radios.Stop();
    media.Stop();
    drag = Id::None;
    outsOpen = false;
    if (hwnd) { KillTimer(hwnd, kTimerClose); DestroyWindow(hwnd); }
    hwnd = nullptr;
    if (surf) surf.Close();
    surf = nullptr; content = nullptr; pill = nullptr; root = nullptr; target = nullptr; clip = nullptr; rim = nullptr;
    fTime = fDate = fTitle = fSub = fSubR = fBatt = fIcon = fIconS = fPill = nullptr;
    dw = nullptr;
    items.clear();
    st = St::Closed;
}

void ControlCenter::Impl::Animate(bool in) {
    const float w = kW * s;
    pill.CenterPoint({w, 0, 0});
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
    ssid = svc::ConnectedSsid();
    bright = svc::brightness::Supported() ? svc::brightness::Level() : -1;
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
            break;
        case Id::Bluetooth:
            if (!radios.HasBluetooth()) return;
            btOn = !btOn;
            radios.SetBluetooth(btOn);
            break;
        case Id::Dnd:
            dnd = !dnd;
            svc::SetDnd(dnd);
            break;
        case Id::Mic:
            if (!mic.Available()) return;
            micMuted = !micMuted;
            mic.SetMute(micMuted);
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
    const float x0 = kPad, x1 = kW - kPad, iw = x1 - x0;
    float y = kPad;
    items.push_back({Id::Header, {x0, y, x1, y + kHeaderH}});
    if (mode != svc::PowerMode::Unknown) {
        float pw = TextWidth(svc::PowerModeName(mode), fPill.get()) + 22;
        float py = y + (battery.hasBattery ? 34 : 20);
        items.push_back({Id::Power, {x1 - pw, py, x1, py + 24}});
    }
    y += kHeaderH + kGap;
    const float tw = (iw - kGap) / 2;
    Id tiles[] = {Id::Wifi, Id::Bluetooth, Id::Dnd, Id::Mic};
    for (int i = 0; i < 4; i++) {
        float tx = x0 + (i % 2) * (tw + kGap), ty = y + (i / 2) * (kTileH + kGap);
        items.push_back({tiles[i], {tx, ty, tx + tw, ty + kTileH}});
    }
    y += 2 * kTileH + kGap + kGap;
    if (bright >= 0) { items.push_back({Id::Bright, {x0, y, x1, y + kSliderH}}); y += kSliderH + kGap; }
    if (speaker.Available()) {
        items.push_back({Id::Vol, {x0, y, x1, y + kSliderH}});
        y += kSliderH + 4;
    }
    if (!outs.empty()) {
        items.push_back({Id::OutRow, {x0, y, x1, y + kOutRowH}});
        y += kOutRowH;
        if (outsOpen)
            for (size_t i = 0; i < outs.size(); i++) {
                items.push_back({Id::OutDev, {x0, y, x1, y + kOutDevH}, static_cast<int>(i)});
                y += kOutDevH;
            }
    }
    y += kGap;
    if (media.State().has) {
        items.push_back({Id::Media, {x0, y, x1, y + kMediaH}});
        float bx = x1 - 6 - 34;
        items.push_back({Id::Next, {bx, y + 16, bx + 34, y + 50}});
        items.push_back({Id::PlayPause, {bx - 34, y + 16, bx, y + 50}});
        items.push_back({Id::Prev, {bx - 68, y + 16, bx - 34, y + 50}});
        y += kMediaH + kGap;
    }
    height = std::min(y - kGap + kPad, kMaxH);
}

// Window, visuals and surface all follow `height`.
void ControlCenter::Impl::ApplySize() {
    int wPx = static_cast<int>(std::ceil(kW * s)), hPx = static_cast<int>(std::ceil(height * s));
    RECT r;
    GetWindowRect(hwnd, &r);
    if (r.bottom - r.top == hPx) return;
    SetWindowPos(hwnd, nullptr, 0, 0, wPx, hPx, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    pill.Size({static_cast<float>(wPx), static_cast<float>(hPx)});
    content.Size({static_cast<float>(wPx), static_cast<float>(hPx)});
    clip.Size({static_cast<float>(wPx), static_cast<float>(hPx)});
    rim.Size({wPx - s, hPx - s});
    surf.as<ICompositionDrawingSurfaceInterop>()->Resize({wPx, hPx});
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

        auto box = [&](D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
            br->SetColor(c);
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br.get());
        };
        auto txt = [&](std::wstring const& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
            br->SetColor(c);
            dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, r, br.get());
        };
        auto glyph = [&](wchar_t g, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) { txt(std::wstring(1, g), f, r, c); };
        auto inset = [](D2D1_RECT_F r, float dx, float dy) { return D2D1::RectF(r.left + dx, r.top + dy, r.right - dx, r.bottom - dy); };

        for (auto const& it : items) {
            auto const& r = it.r;
            switch (it.id) {
                case Id::Header: {
                    SYSTEMTIME lt{};
                    GetLocalTime(&lt);
                    wchar_t t[32] = {}, d[64] = {};
                    GetTimeFormatEx(nullptr, TIME_NOSECONDS, &lt, nullptr, t, 32);
                    GetDateFormatEx(nullptr, 0, &lt, L"dddd, d MMMM", d, 64, nullptr);
                    txt(t, fTime.get(), {r.left + 2, r.top - 2, r.right - 120, r.top + 38}, kText);
                    txt(d, fDate.get(), {r.left + 3, r.top + 38, r.right - 120, r.top + 58}, kDim);
                    if (battery.hasBattery) {
                        int lvl = std::min(10, battery.percent / 10);
                        wchar_t g = static_cast<wchar_t>((battery.ac ? 0xEBAB : 0xEBA0) + lvl);
                        std::wstring pct = std::to_wstring(battery.percent) + L"%";
                        float pw = TextWidth(pct, fBatt.get());
                        txt(pct, fBatt.get(), {r.right - pw - 30, r.top + 4, r.right, r.top + 28}, kText);
                        glyph(g, fIcon.get(), {r.right - pw - 34 - 8, r.top + 4, r.right - pw - 6, r.top + 28}, kText);
                    }
                    break;
                }
                case Id::Power:
                    box(r, 12, kTile);
                    txt(svc::PowerModeName(mode), fPill.get(), r, kText);
                    break;
                case Id::Wifi: case Id::Bluetooth: case Id::Dnd: case Id::Mic: {
                    bool avail = true, on = false;
                    wchar_t g = 0;
                    std::wstring title, sub;
                    if (it.id == Id::Wifi) {
                        avail = radios.HasWifi(); on = wifiOn; g = 0xE701; title = L"Wi-Fi";
                        sub = !avail ? L"Unavailable" : !on ? L"Off" : ssid.empty() ? L"Not connected" : ssid;
                    } else if (it.id == Id::Bluetooth) {
                        avail = radios.HasBluetooth(); on = btOn; g = 0xE702; title = L"Bluetooth";
                        sub = !avail ? L"Unavailable" : on ? L"On" : L"Off";
                    } else if (it.id == Id::Dnd) {
                        on = dnd; g = 0xE708; title = L"Do Not Disturb"; sub = on ? L"On" : L"Off";
                    } else {
                        avail = mic.Available(); on = avail && !micMuted; g = 0xE720; title = L"Microphone";
                        sub = !avail ? L"Unavailable" : on ? L"On" : L"Muted";
                    }
                    float a = avail ? 1.0f : 0.45f;
                    box(r, kTileRadius, kTile);
                    float cx = r.left + 10 + 16, cy = (r.top + r.bottom) / 2;
                    br->SetColor(on ? ColorF(0x0A84FF, a) : ColorF(1, 1, 1, 0.16f * a));
                    dc->FillEllipse(D2D1::Ellipse({cx, cy}, 16, 16), br.get());
                    glyph(g, fIconS.get(), {cx - 16, cy - 16, cx + 16, cy + 16}, ColorF(1, 1, 1, a));
                    if (it.id == Id::Mic && avail && micMuted) {
                        br->SetColor(ColorF(1, 1, 1, 0.95f));
                        dc->DrawLine({cx - 7, cy - 7}, {cx + 7, cy + 7}, br.get(), 1.6f);
                    }
                    txt(title, fTitle.get(), {r.left + 50, cy - 18, r.right - 6, cy}, ColorF(1, 1, 1, 0.94f * a));
                    txt(sub, fSub.get(), {r.left + 50, cy + 1, r.right - 6, cy + 19}, ColorF(1, 1, 1, 0.58f * a));
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
                    txt(name, fSub.get(), {r.left + 6, r.top + 4, r.right - 30, r.bottom}, kDim);
                    glyph(outsOpen ? 0xE70E : 0xE70D, fIconS.get(), {r.right - 28, r.top, r.right - 4, r.bottom}, kDim);
                    break;
                }
                case Id::OutDev: {
                    auto const& d = outs[it.arg];
                    if (d.isDefault) box(inset(r, 0, 1), 8, kTile);
                    txt(d.name, fSub.get(), {r.left + 10, r.top + 6, r.right - 34, r.bottom}, d.isDefault ? kText : kDim);
                    if (d.isDefault) glyph(0xE73E, fIconS.get(), {r.right - 30, r.top, r.right - 6, r.bottom}, kAccent);
                    break;
                }
                case Id::Media: {
                    auto const& ms = media.State();
                    box(r, kTileRadius, kTile);
                    txt(ms.title.empty() ? L"Now playing" : ms.title, fTitle.get(), {r.left + 14, r.top + 13, r.right - 120, r.top + 33}, kText);
                    txt(ms.artist, fSub.get(), {r.left + 14, r.top + 34, r.right - 120, r.top + 54}, kDim);
                    break;
                }
                case Id::Prev: case Id::PlayPause: case Id::Next: {
                    auto const& ms = media.State();
                    bool ok = it.id == Id::Prev ? ms.canPrev : it.id == Id::Next ? ms.canNext : true;
                    wchar_t g = it.id == Id::Prev ? 0xE892 : it.id == Id::Next ? 0xE893 : (ms.playing ? 0xE769 : 0xE768);
                    glyph(g, fIcon.get(), r, ColorF(1, 1, 1, ok ? 0.94f : 0.3f));
                    break;
                }
                default: break;
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
    mk(fTime, L"Segoe UI Variable Display", SB, 30, L, N);
    mk(fDate, L"Segoe UI Variable Text", R, 12.5f, L, N);
    mk(fTitle, L"Segoe UI Variable Text", SB, 12.5f, L, N);
    mk(fSub, L"Segoe UI Variable Text", R, 11.5f, L, N);
    mk(fSubR, L"Segoe UI Variable Text", R, 11.5f, T, N);
    mk(fBatt, L"Segoe UI Variable Text", SB, 14, T, M);
    mk(fIcon, L"Segoe Fluent Icons", R, 17, C, M);
    mk(fIconS, L"Segoe Fluent Icons", R, 14, C, M);
    mk(fPill, L"Segoe UI Variable Text", SB, 11.5f, C, M);
}

// Same recipe as the dock pill (Halo::BuildPill): blurred backdrop, #16161A tint at 50%, 2.5% grain,
// a 1 px gradient rim, radius 20.
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
    : m(std::make_unique<Impl>(inst, owner, std::move(comp), std::move(gdev))) {
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

}  // namespace halo
