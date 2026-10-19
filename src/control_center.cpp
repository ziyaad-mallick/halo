#include "control_center.h"

#include <d2d1_1.h>
#include <wincodec.h>
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
#include "theme.h"
#include "tokens.h"

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
// Geometry, all from tokens.h: a 344 px panel, the inset and gaps on the 4 px scale, concentric corners.
constexpr float kW = 380, kInset = ui::kPanelInset, kGap = ui::kS3, kRadius = ui::kPanelRadius;
constexpr float kHeaderH = 56, kTitleLine = 20, kCaptionLine = 16, kListRowH = 36, kMediaH = 76, kPowH = 52;
constexpr float kMaxH = 900, kMargin = ui::kS2;
constexpr int kMaxOutputs = 6;

constexpr UINT kMsgAudio = WM_APP + 1, kMsgMic = WM_APP + 2, kMsgRadios = WM_APP + 3, kMsgBright = WM_APP + 4,
               kMsgMedia = WM_APP + 5, kMsgWifi = WM_APP + 6, kMsgBt = WM_APP + 7, kMsgLate = WM_APP + 8,
               kMsgTestClick = WM_APP + 30;
constexpr UINT_PTR kTimerClose = 1, kTimerChord = 2, kTimerClock = 3, kTimerLeave = 4, kTimerSettle = 5;
// While the opening spring runs nothing redraws: updates that arrive (the deferred reads, radios, media,
// brightness) are held and drawn once, kSettleMs after the show. A redraw mid-motion cost a frame.
constexpr UINT kSettleMs = 330;
// Auto-hide: while open, where the pointer is gets checked every kLeaveTickMs; kLeaveMs outside the panel
// (plus kLeaveSlack px around it) closes it. Then the chips: a hovered control grows by about kLiftPx
// logical px and leans toward the pointer by kLean of the distance, at most kLeanMax.
constexpr UINT kLeaveTickMs = 50, kLeaveMs = 280;
constexpr float kLeaveSlack = 18, kLiftPx = 4, kPressScale = 0.96f, kLean = 0.07f, kLeanMax = 3.5f;
constexpr wchar_t kPanelClass[] = L"HaloControlCenter", kHotClass[] = L"HaloControlCenterTimers";

enum class Id {
    None, WifiPill, BtPill, WifiText, BtText, Wifi, Bluetooth, RowTile, Airplane, Hotspot, Night, Dark, Dnd,
    DisplayTile, Bright, SoundTile, Vol, OutRow, OutDev, ListTile, WifiNet, BtDev, MediaTile, Prev, PlayPause, Next,
    Snip, Cast, Settings, Battery, Lock, Power, Mic, PowerTile, PowSleep, PowRestart, PowShut, Clock, Label
};
bool Passive(Id id) {
    return id == Id::WifiPill || id == Id::BtPill || id == Id::RowTile || id == Id::DisplayTile || id == Id::SoundTile ||
           id == Id::ListTile || id == Id::MediaTile || id == Id::PowerTile || id == Id::Label;
}
// Controls that grow under the pointer and squash under the finger (each is drawn on its own little
// surface, a chip).
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

// Ink and fills come from the shared palette (light by default, HALO_THEME=dark for the old look). These
// refer into the live palette, so a theme or accent change from settings.ini shows at the next paint.
const D2D1_COLOR_F &kText = theme::detail::Cur().text, &kDim = theme::detail::Cur().dim, &kOff = theme::detail::Cur().off,
                   &kAccent = theme::detail::Cur().accent, &kOnAccent = theme::detail::Cur().onAccent;

// A soft drop shadow that follows a rounded-rect mask. Lives under the clipped glass, so it
// must be a sibling of the clipped container, not a child.
wuc::SpriteVisual MakeShadow(wuc::Compositor const& comp, wuc::CompositionGraphicsDevice const& gdev, float wPx, float hPx,
                             float radPx, float blur, float dy, theme::Argb col) {
    if (col.a == 0 || blur <= 0) return nullptr;
    try {
        auto surface = gdev.CreateDrawingSurface({wPx, hPx}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                 winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        auto interop = surface.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return nullptr;
        dc->Clear(ColorF(0, 0.0f));
        winrt::com_ptr<ID2D1SolidColorBrush> b;
        dc->CreateSolidColorBrush(ColorF(1, 1, 1, 1), b.put());
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(off.x + 0.0f, off.y + 0.0f, off.x + wPx, off.y + hPx), radPx, radPx), b.get());
        interop->EndDraw();
        auto surfaceBrush = comp.CreateSurfaceBrush(surface);
        surfaceBrush.Stretch(wuc::CompositionStretch::Fill);
        auto mask = comp.CreateNineGridBrush();  // corners stay round while the height animates
        mask.Source(surfaceBrush);
        mask.SetInsets(radPx + 1);
        auto sh = comp.CreateDropShadow();
        sh.BlurRadius(blur);
        sh.Color(theme::C(col));
        sh.Offset({0, dy, 0});
        sh.Mask(mask);
        auto v = comp.CreateSpriteVisual();
        v.Size({wPx, hPx});
        v.Brush(mask);
        v.Shadow(sh);
        return v;
    } catch (...) { return nullptr; }
}

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
    wuc::ContainerVisual root{nullptr}, pill{nullptr}, inner{nullptr}, fx{nullptr};
    wuc::SpriteVisual shadowV{nullptr};
    wuc::SpriteVisual content{nullptr};
    wuc::ShapeVisual hv{nullptr};
    wuc::CompositionDrawingSurface surf{nullptr};
    // Height changes: the window keeps the panel's tallest size and never moves; the frame springs to the new
    // height, growing upward from the fixed bottom edge. The drawing is shown in two parts split where the layout
    // changed: `content` above the split rides the top edge, `lower` below it rides the bottom edge, and what opens
    // between them (a list, the Power row) is uncovered like a drawer; what closes is the old drawing (ghost),
    // covered the same way.
    wuc::SpriteVisual lower{nullptr}, ghost{nullptr};
    wuc::InsetClip upperClip{nullptr}, lowerClip{nullptr};
    float split = 0;    // logical y in the new layout where it changed: what is above kept its place from the top
    float shownPx = 0;  // the panel height (px) the frame is at or springing to
    struct Chip {
        Item it;
        wuc::SpriteVisual v{nullptr};
        wuc::CompositionDrawingSurface surf{nullptr};
        wfn::float3 rest{};  // offset in the pill, px
        wfn::float2 size{};
        float lift = 1;      // hover scale
    };
    std::vector<Chip> chips;
    wuc::ContainerVisual chipLayer{nullptr};
    wuc::CompositionPropertySet ptr{nullptr};  // "P": the pointer in pill px, read by the hovered chip's lean
    int hotChip = -1;
    wuc::CompositionRoundedRectangleGeometry clip{nullptr}, rim{nullptr}, hvGeo{nullptr}, specGeo{nullptr};
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
    size_t listCap = SIZE_MAX;  // the most list rows Layout() may show (it lowers this when the panel would not fit)
    float volume = 0;
    bool muted = false, micMuted = false, dnd = false, wifiOn = false, btOn = false, dark = false, energy = false, night = false;
    bool hotspotAvail = false, hotspotOn = false, savedWifi = true, savedBt = true, btAsked = false;
    int bright = -1;
    svc::Battery battery;
    svc::PowerMode mode = svc::PowerMode::Unknown;
    std::wstring ssid;
    std::vector<Item> items;
    Item hover, pressed;
    bool tracking = false, synced = false, settling = false, held = false, needSync = false;
    bool byPointer = false, entered = false;  // opened from the screen edge; the pointer has been over the panel
    ULONGLONG outSince = 0;
    bool powerOpen = false;
    WORD lastMin = 0xFFFF;
    Id drag = Id::None;
    ULONGLONG lastBrightSet = 0;
    WORD chordKey = 0;
    bool chordShift = false;


    Impl(ControlCenter* a, HINSTANCE i, HWND o, wuc::Compositor c, wuc::CompositionGraphicsDevice g)
        : api(a), inst(i), owner(o), comp(std::move(c)), gdev(std::move(g)) {}

    void RegisterClasses();
    void Open(bool pointer);
    void Close();
    void Teardown();
    void Park();
    float builtScale = 0;  // the DPI scale the window and visuals were built for
    bool stale = false;    // settings changed the glass: rebuild the window and visuals on the next open
    settings::ControlCenter const& cfg() const { return settings::Get().cc; }
    void CheckPointer();
    void Snapshot();
    // It rises a little as it fades in, from the bottom edge it lives on (and sinks back out).
    wfn::float2 Travel() const { return {0, ui::kEnterTravel * s}; }
    wfn::float2 Corner() const { return {1, 1}; }  // the swoop grows from the bottom right, where it opens
    void Settle();
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
    void SyncChips();
    int ChipOf(Item const* it) const;
    void HoverChip(int i);
    void Squash(bool down);
    void Click(Item const& it);
    void LaunchAfterClose(wchar_t const* uri);
    void DeferChord(WORD key, bool shift);
    void DragTo(float x);
    void RunPowerAfterClose(int item);
    Item const* HitAt(float x, float y) const;
    float TextWidth(std::wstring const& t, IDWriteTextFormat* f);
    float MaxPx() const { return std::ceil(MaxH() * s); }
    float TopPx() const { return std::lround(motion::kPad * s) + MaxPx() - shownPx; }  // the pill's y in the window
    void Resize(float hPx, bool animate);
    float MaxH() const { return std::min(kMaxH, std::max(200.0f, (mon.bottom - mon.top) / s - 2 * kMargin)); }
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
    if (self && m == kMsgMedia) { self->media.Pull(); return 0; }
    if (self && (m == kMsgAudio || m == kMsgMic || m == kMsgRadios || m == kMsgBt)) {
        try {
            return self->PanelProc(h, m, w, l);
        } catch (winrt::hresult_error const& e) {  // posted by foreign-thread events: nothing above us catches
            svc::Log(L"hot message " + std::to_wstring(m) + L" failed: " + std::wstring(e.message().c_str()));
            return 0;
        }
    }
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
    const float x = static_cast<short>(LOWORD(l)) / s - motion::kPad, y = (static_cast<short>(HIWORD(l)) - TopPx()) / s;
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
            Settle();  // a click shows its result at once
            if (x < 0 || y < 0 || x >= kW || y >= height) { Close(); return 0; }  // the transparent margin is "outside"
            if (auto* it = HitAt(x, y)) {
                if (it->id == Id::Bright || it->id == Id::Vol) {
                    drag = it->id;
                    SetCapture(hwnd);
                    DragTo(x);
                } else {
                    pressed = *it;
                    if (Scalable(it->id)) { HoverChip(ChipOf(it)); Squash(true); }
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
            if (ptr) ptr.InsertVector2(L"P", {x * s, y * s});
            auto* it = HitAt(x, y);
            Item now = it ? *it : Item{};
            if (!Same(now, hover)) {
                hover = now;
                SetHover(it);
                if (pressed.id == Id::None) HoverChip(ChipOf(it));
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            if (hover.id != Id::None) { hover = {}; SetHover(nullptr); }
            if (pressed.id == Id::None) HoverChip(-1);
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
                if (Scalable(p.id)) Squash(false);
                if (it && Same(*it, p)) {
                    Item c = *it;  // Click re-lays-out and invalidates `it`
                    Click(c);
                }
                if (st == St::Open) HoverChip(ChipOf(hover.id != Id::None ? &hover : nullptr));
            }
            return 0;
        case kMsgTestClick:  // HALO_TEST: wParam = an Id, clicked as if by the pointer
            if (st == St::Open && _wgetenv(L"HALO_TEST"))
                for (auto const& it : items)
                    if (static_cast<WPARAM>(it.id) == w) { Item c = it; Settle(); Click(c); Snapshot(); break; }
            return 0;
        case kMsgAudio: SyncSpeaker(); Render(); return 0;
        case kMsgMic: SyncMic(); Render(); return 0;
        case kMsgRadios:
            SyncRadios();
            if (btOn && !btAsked) { btAsked = true; svc::BtRefresh(hot, kMsgBt); }  // for "connected device" under Bluetooth
            Render();
            return 0;
        case kMsgBright:
            if (svc::brightness::Supported()) bright = svc::brightness::Level();
            Render();
            return 0;
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
            if (w == kTimerClose) Park();
            else if (w == kTimerLeave) CheckPointer();
            else if (w == kTimerSettle) Settle();
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
    return {static_cast<LONG>(std::ceil(kW * s)) + 2 * pad, static_cast<LONG>(MaxPx()) + 2 * pad};
}

void ControlCenter::Impl::Open(bool pointer) {
    if (st == St::Open) return;
    byPointer = pointer;
    entered = false;
    outSince = 0;
    if (st == St::Closing) Park();
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
        // The window and its visual tree are built once and kept hidden between opens (Park): a new
        // backdrop window per open cost DWM the first frames of the slide. Rebuilt only for a new DPI.
        if (hwnd && (builtScale != s || stale)) Teardown();
        stale = false;
        if (!hwnd) {
            hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kPanelClass,
                                   L"Halo Control Center", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
            if (!hwnd) return;
            BOOL on = TRUE;
            DwmSetWindowAttribute(hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));
            builtScale = s;
        }
        // The reads (audio endpoints above all) cost ~20 ms: after the first open the panel shows with
        // what it read last time and syncs once the motion has settled.
        needSync = synced;
        if (!synced) SyncAll();
        synced = true;
        Layout();
        const int pad = static_cast<int>(std::lround(motion::kPad * s)), mg = static_cast<int>(std::lround(kMargin * s));
        int wPx = static_cast<int>(std::ceil(kW * s)), hPx = static_cast<int>(std::ceil(height * s));
        rect = {mon.right - wPx - mg, mon.bottom - mg - hPx, 0, 0};  // the bottom right, where Quick Settings opens
        rect.right = rect.left + wPx;
        rect.bottom = rect.top + hPx;
        SIZE wp = WindowPx();
        SetWindowPos(hwnd, HWND_TOPMOST, rect.left - pad, rect.bottom + pad - wp.cy, wp.cx, wp.cy, SWP_NOACTIVATE);
        shownPx = static_cast<float>(hPx);
        if (!pill) BuildVisuals();
        Resize(static_cast<float>(hPx), false);
        st = St::Open;
        Render();
        motion::In(comp, pill, {pad * 1.0f, TopPx(), 0}, Travel(), cfg(), Corner());  // before the show: no frame at rest first
        // From the edge the panel only shows (the app under the pointer keeps the keyboard, and nothing
        // waits on a foreground change); a click in it activates it like any window.
        ShowWindow(hwnd, pointer ? SW_SHOWNOACTIVATE : SW_SHOW);
        settling = true;
        SetTimer(hwnd, kTimerSettle, kSettleMs, nullptr);
        {
            LARGE_INTEGER q1;
            QueryPerformanceCounter(&q1);
            svc::Log(L"open latency Open()->ShowWindow: " + std::to_wstring((q1.QuadPart - q0.QuadPart) * 10000 / qf.QuadPart / 10.0) + L" ms");
        }
        if (!pointer) Focus();
        SetTimer(hwnd, kTimerClock, 15000, nullptr);
        SetTimer(hwnd, kTimerLeave, kLeaveTickMs, nullptr);
        // Async sources fill in as they arrive (the radios and audio are followed all the time, from the ctor).
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
    settling = held = needSync = false;
    st = St::Closing;
    KillTimer(hwnd, kTimerLeave);
    KillTimer(hwnd, kTimerSettle);
    // The scans stop; the radio and volume listeners stay, so the next open already shows the truth.
    svc::WifiStop();
    svc::BtStop();
    SetHover(nullptr);
    hover = pressed = {};
    drag = Id::None;
    const float pad = motion::kPad * s;
    motion::Out(comp, pill, {pad, TopPx(), 0}, Travel(), cfg(), Corner());
    SetTimer(hwnd, kTimerClose, motion::OutMs(cfg()) + 10, nullptr);
    if (api->onHidden) api->onHidden();
}

// Closed but kept: the window hides, the visuals and surfaces stay for the next open.
void ControlCenter::Impl::Park() {
    bool wasOpen = st == St::Open;
    svc::WifiStop();
    svc::BtStop();
    drag = Id::None;
    outsOpen = false;
    powerOpen = false;
    expand = Expand::None;
    hover = pressed = {};
    tracking = false;
    if (hv) { hv.StopAnimation(L"Opacity"); hv.Opacity(0); }
    hvShown = false;
    if (ghost) { inner.Children().Remove(ghost); ghost = nullptr; }
    for (auto& c : chips) {  // back to rest, so the next open does not start with a lifted chip
        c.v.StopAnimation(L"Scale");
        c.v.StopAnimation(L"Offset");
        c.v.Scale({1, 1, 1});
        c.v.Offset(c.rest);
    }
    hotChip = -1;
    if (ptr) ptr.InsertVector2(L"P", {-1000, -1000});
    if (hwnd) {
        KillTimer(hwnd, kTimerClose);
        ShowWindow(hwnd, SW_HIDE);
    }
    st = St::Closed;
    rect = {};
    if (wasOpen && api->onHidden) api->onHidden();
}

// Releases the window and every composition object (the DirectWrite objects and grain stay cached).
void ControlCenter::Impl::Teardown() {
    bool wasOpen = st == St::Open;
    svc::WifiStop();
    svc::BtStop();
    drag = Id::None;
    outsOpen = false;
    powerOpen = false;
    expand = Expand::None;
    hover = pressed = {};
    tracking = false;
    hvShown = false;
    st = St::Closed;  // before DestroyWindow: its WM_ACTIVATE must not find the panel Open and Close() it again
    if (hwnd) { KillTimer(hwnd, kTimerClose); DestroyWindow(hwnd); }
    hwnd = nullptr;
    try {  // Close() throws once the compositor is gone (exit); this also runs from the destructor
        if (surf) surf.Close();
        for (auto& c : chips) if (c.surf) c.surf.Close();
    } catch (winrt::hresult_error const&) {}
    chips.clear();
    hotChip = -1;
    shownPx = 0;
    ghost = lower = nullptr;
    upperClip = lowerClip = nullptr;
    surf = nullptr; content = nullptr; chipLayer = nullptr; ptr = nullptr; hv = nullptr; hvGeo = nullptr;
    pill = nullptr; inner = nullptr; shadowV = nullptr; fx = nullptr; root = nullptr; target = nullptr; clip = nullptr; rim = nullptr; specGeo = nullptr;
    items.clear();
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
    shape.StrokeBrush(comp.CreateColorBrush(theme::Dark() ? winrt::Windows::UI::ColorHelper::FromArgb(0xB0, 0xFF, 0xFF, 0xFF)
                                                         : winrt::Windows::UI::ColorHelper::FromArgb(0x90, 0x0F, 0x76, 0x6E)));
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
    bool show = it && it->id != Id::Bright && it->id != Id::Vol && !Scalable(it->id);  // a chip is its own highlight
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

// Hover and press feedback, the dock's way: every Scalable control is drawn onto its own little surface
// (a chip, like a dock icon), so hovering never redraws anything. The hovered chip springs up by about
// kLiftPx and leans toward the pointer through an expression the compositor evaluates from `ptr`
// (no animation restarts on mouse moves); it squashes to kPressScale under the finger and bounces back.
void ControlCenter::Impl::SyncChips() {
    if (!chipLayer) return;
    std::vector<Chip> next;
    for (auto const& it : items) {
        if (!Scalable(it.id)) continue;
        const int l = static_cast<int>(std::floor(it.r.left * s)), t = static_cast<int>(std::floor(it.r.top * s));
        const int w = static_cast<int>(std::ceil(it.r.right * s)) - l, h = static_cast<int>(std::ceil(it.r.bottom * s)) - t;
        Chip c;
        for (auto& old : chips)  // the same control keeps its visual (and its animation); moved, it glides there
            if (old.v && Same(old.it, it) && old.size.x == w && old.size.y == h) {
                c = std::move(old);
                old.v = nullptr;
                if (c.rest.x != l || c.rest.y != t) {
                    c.rest = {static_cast<float>(l), static_cast<float>(t), 0};
                    auto glide = comp.CreateSpringVector3Animation();
                    glide.FinalValue(winrt::Windows::Foundation::IReference<wfn::float3>(c.rest));
                    glide.DampingRatio(0.86f);
                    glide.Period(std::chrono::milliseconds(48));
                    c.v.StartAnimation(L"Offset", glide);
                }
                break;
            }
        if (!c.v) {
            c.surf = gdev.CreateDrawingSurface({static_cast<float>(w), static_cast<float>(h)},
                                               winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                               winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            c.v = comp.CreateSpriteVisual();
            c.v.Size({static_cast<float>(w), static_cast<float>(h)});
            c.rest = {static_cast<float>(l), static_cast<float>(t), 0};
            c.size = {static_cast<float>(w), static_cast<float>(h)};
            c.v.Offset(c.rest);
            c.v.CenterPoint({w / 2.0f, h / 2.0f, 0});
            auto sb = comp.CreateSurfaceBrush(c.surf);
            sb.Stretch(wuc::CompositionStretch::None);
            c.v.Brush(sb);
            chipLayer.Children().InsertAtTop(c.v);
        }
        c.it = it;
        c.lift = std::clamp(1 + kLiftPx / std::min(it.r.right - it.r.left, it.r.bottom - it.r.top), 1.03f, 1.09f);
        auto interop = c.surf.as<ICompositionDrawingSurfaceInterop>();
        winrt::com_ptr<ID2D1DeviceContext> dc;
        POINT off{};
        if (SUCCEEDED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) {
            try {
                Paint(dc.get(), off.x - c.rest.x, off.y - c.rest.y, &it, nullptr);
            } catch (...) {}
            interop->EndDraw();
        }
        next.push_back(std::move(c));
    }
    for (auto& old : chips)
        if (old.v) {
            chipLayer.Children().Remove(old.v);
            if (old.surf) old.surf.Close();
        }
    const Item was = hotChip >= 0 && hotChip < static_cast<int>(chips.size()) ? chips[hotChip].it : Item{};
    chips = std::move(next);
    hotChip = -1;
    for (size_t i = 0; i < chips.size(); i++)
        if (was.id != Id::None && Same(chips[i].it, was)) hotChip = static_cast<int>(i);
}

int ControlCenter::Impl::ChipOf(Item const* it) const {
    if (!it || !Scalable(it->id)) return -1;
    for (size_t i = 0; i < chips.size(); i++)
        if (Same(chips[i].it, *it)) return static_cast<int>(i);
    return -1;
}

void ControlCenter::Impl::HoverChip(int i) {
    if (i == hotChip) return;
    if (hotChip >= 0 && hotChip < static_cast<int>(chips.size())) {  // the old one settles back
        auto& c = chips[hotChip];
        auto scale = comp.CreateSpringVector3Animation();
        scale.FinalValue(Ref3(1.0f));
        scale.DampingRatio(0.6f);
        scale.Period(45ms);
        c.v.StartAnimation(L"Scale", scale);
        auto back = comp.CreateSpringVector3Animation();
        back.FinalValue(winrt::Windows::Foundation::IReference<wfn::float3>(c.rest));
        back.DampingRatio(0.6f);
        back.Period(45ms);
        c.v.StartAnimation(L"Offset", back);
    }
    hotChip = i;
    if (i < 0) return;
    auto& c = chips[i];
    auto grow = comp.CreateSpringVector3Animation();
    grow.FinalValue(Ref3(c.lift));
    grow.DampingRatio(0.55f);
    grow.Period(45ms);
    c.v.StartAnimation(L"Scale", grow);
    auto lean = comp.CreateExpressionAnimation(
        L"Vector3(rest.X + Clamp((ptr.P.X - mid.X) * k, -m, m), rest.Y + Clamp((ptr.P.Y - mid.Y) * k, -m, m), 0)");
    lean.SetReferenceParameter(L"ptr", ptr);
    lean.SetVector3Parameter(L"rest", c.rest);
    lean.SetVector2Parameter(L"mid", {c.rest.x + c.size.x / 2, c.rest.y + c.size.y / 2});
    lean.SetScalarParameter(L"k", kLean);
    lean.SetScalarParameter(L"m", kLeanMax * s);
    c.v.StartAnimation(L"Offset", lean);
}

void ControlCenter::Impl::Squash(bool down) {
    if (hotChip < 0 || hotChip >= static_cast<int>(chips.size())) return;
    auto& c = chips[hotChip];
    auto spring = comp.CreateSpringVector3Animation();
    spring.FinalValue(Ref3(down ? kPressScale : c.lift));
    spring.DampingRatio(down ? 0.55f : 0.42f);  // the release overshoots: the bounce
    spring.Period(down ? 35ms : 50ms);
    c.v.StartAnimation(L"Scale", spring);
}

// Auto-hide. Inside: the panel (and kLeaveSlack around it), the notification column, and, until the
// pointer has first come over the panel, the screen-edge hot zone it came from. A slider drag holds it.
void ControlCenter::Impl::CheckPointer() {
    if (st != St::Open || !cfg().closeOnLeave || drag != Id::None || (GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return;
    POINT p;
    if (!GetCursorPos(&p)) return;
    if (PtInRect(&rect, p)) entered = true;
    if (!byPointer && !entered) return;  // opened from the keyboard: it goes once the pointer has been and gone
    RECT r = rect;
    const int slack = static_cast<int>(kLeaveSlack * s);
    InflateRect(&r, slack, slack);
    bool in = PtInRect(&r, p);
    if (!in && api->siblingHwnd) {
        if (HWND sib = api->siblingHwnd(); sib && IsWindowVisible(sib)) {
            RECT sr;
            in = GetWindowRect(sib, &sr) && PtInRect(&sr, p);
        }
    }
    if (!in && !entered) {
        const long w = mon.right - mon.left;
        in = p.y >= mon.bottom - 1 - slack && p.x >= mon.right - w * cfg().cornerWidth / 100;
    }
    if (in) { outSince = 0; return; }
    const ULONGLONG now = GetTickCount64();
    if (!outSince) outSince = now;
    else if (now - outSince >= kLeaveMs) {
        svc::Log(L"pointer left, closing");
        Close();
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
    ssid = wifiOn ? svc::ConnectedSsid() : std::wstring();
}

// Only fast reads here: the hotspot and SSID queries (20+ ms between them) come in through StartLate.
void ControlCenter::Impl::SyncAll() {
    speaker.Bind(eRender, hot, kMsgAudio);  // re-bound each sync: the default device may have changed
    mic.Bind(eCapture, hot, kMsgMic);
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
        case Id::Clock:
            if (api->onClock) api->onClock();
            break;
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
            if (!svc::DryRun()) { speaker.Bind(eRender, hot, kMsgAudio); SyncSpeaker(); }
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
    using namespace ui;
    items.clear();
    const float x0 = kInset, x1 = kW - kInset;
    float y = kInset;
    auto add = [&](Id id, D2D1_RECT_F r, int arg = 0, float rad = kRadiusSmall) { items.push_back({id, r, arg, rad}); };

    // Header, on the glass itself: the time and date (opens the calendar), the battery chip (cycles the power mode).
    add(Id::Clock, {x0, y, x0 + 220, y + kHeaderH}, 0, kRadiusSmall);
    add(Id::Battery, {x1 - 136, y + kS2, x1, y + kHeaderH - kS2}, 0, kRadiusSmall);
    y += kHeaderH + kGap;

    // The toggle grid: three rows of 64 px on two columns.
    //   Wi-Fi pill            | Bluetooth pill          (round toggle; the text side opens the list)
    //   Airplane | Hotspot    | Do not disturb pill     (icon-only tiles)
    //   Night light pill      | Dark mode pill
    const float col = (x1 - x0 - kGap) / 2, half = (col - kGap) / 2, rx = x0 + col + kGap;
    auto pill = [&](float l, float top, Id back, Id dot, Id text) {
        const float r = l + col, icx = l + kS3 + kRound / 2, icy = top + kPillH / 2;
        add(back, {l, top, r, top + kPillH}, 0, kRadiusLarge);
        add(text, {icx + kRound / 2 + kS3, top + kS2, r - kS1, top + kPillH - kS2}, 0, kRadiusSmall);
        add(dot, {icx - kRound / 2, icy - kRound / 2, icx + kRound / 2, icy + kRound / 2}, 0, kRound / 2);
    };
    pill(x0, y, Id::WifiPill, Id::Wifi, Id::WifiText);
    pill(rx, y, Id::BtPill, Id::Bluetooth, Id::BtText);
    y += kPillH + kGap;
    add(Id::Airplane, {x0, y, x0 + half, y + kPillH}, 0, kRadiusLarge);
    add(Id::Hotspot, {x0 + half + kGap, y, x0 + col, y + kPillH}, 0, kRadiusLarge);
    add(Id::Dnd, {rx, y, x1, y + kPillH}, 0, kRadiusLarge);
    y += kPillH + kGap;
    add(Id::Night, {x0, y, x0 + col, y + kPillH}, 0, kRadiusLarge);
    add(Id::Dark, {rx, y, x1, y + kPillH}, 0, kRadiusLarge);
    y += kPillH + kGap;

    // The Wi-Fi / Bluetooth list, when one is open.
    size_t listed = 0;
    if (expand != Expand::None) {
        bool wifi = expand == Expand::Wifi;
        size_t n = std::min(wifi ? svc::WifiNets().size() : svc::BtDevices().size(), listCap);
        listed = n;
        float h = kS4 + kTitleLine + kS1 + std::max<size_t>(1, n) * kListRowH + kS2;
        add(Id::ListTile, {x0, y, x1, y + h}, 0, kRadiusLarge);
        for (size_t i = 0; i < n; i++) {
            float t = y + kS4 + kTitleLine + kS1 + i * kListRowH;
            add(wifi ? Id::WifiNet : Id::BtDev, {x0 + kS2, t, x1 - kS2, t + kListRowH}, static_cast<int>(i), kRadiusSmall);
        }
        y += h + kGap;
    }

    // One module for both sliders: Display, then Sound (whose output list opens under it). The Sound row
    // is always laid out (greyed when there is no output), so the height never depends on the audio read.
    if (cfg().showSliders) {
        const bool disp = bright >= 0;
        const float extra = outsOpen ? outs.size() * kListRowH + kS1 : 0;
        const float blk = kTitleLine + kS2 + kSliderH;  // a label over a track
        const float h = kS4 + (disp ? blk + kS4 : 0) + blk + kS4 + extra;
        add(Id::DisplayTile, {x0, y, x1, y + h}, 0, kRadiusLarge);
        float t = y + kS4;
        if (disp) {
            add(Id::Bright, {x0 + kS4, t + blk - kSliderH, x1 - kS4, t + blk});
            t += blk + kS4;
        }
        add(Id::SoundTile, {x0, t, x1, t + kTitleLine});  // only for the "Sound" label
        add(Id::OutRow, {x0 + 112, t - kS1, x1 - kS2, t + kTitleLine + kS1}, 0, kRadiusSmall);
        add(Id::Vol, {x0 + kS4, t + blk - kSliderH, x1 - kS4, t + blk});
        t += blk + kS4;
        if (outsOpen)
            for (size_t i = 0; i < outs.size(); i++)
                add(Id::OutDev, {x0 + kS2, t + i * kListRowH, x1 - kS2, t + (i + 1) * kListRowH}, static_cast<int>(i), kRadiusSmall);
        y += h + kGap;
    }

    // Now Playing: always there, so the panel never changes height when a session turns up.
    if (cfg().showMedia) {
        add(Id::MediaTile, {x0, y, x1, y + kMediaH}, 0, kRadiusLarge);
        const float cy = y + kMediaH / 2, b = kRound;
        float r = x1 - kS3;
        add(Id::Next, {r - b, cy - b / 2, r, cy + b / 2}, 0, b / 2);
        r -= b + kS1;
        add(Id::PlayPause, {r - b, cy - b / 2, r, cy + b / 2}, 0, b / 2);
        r -= b + kS1;
        add(Id::Prev, {r - b, cy - b / 2, r, cy + b / 2}, 0, b / 2);
        y += kMediaH + kS4;
    }

    // Actions: free-standing round buttons across the width.
    {
        Id ids[] = {Id::Mic, Id::Snip, Id::Cast, Id::Settings, Id::Lock, Id::Power};
        const float d = kRoundAction, step = (x1 - x0 - d) / 5;
        for (int i = 0; i < 6; i++) {
            float l = x0 + i * step;
            add(ids[i], {l, y, l + d, y + d}, 0, d / 2);
        }
        y += d + kGap;
    }
    // The inline Sleep / Restart / Shut down choice under the actions.
    if (powerOpen) {
        add(Id::PowerTile, {x0, y, x1, y + kPowH}, 0, kRadiusLarge);
        Id ids[] = {Id::PowSleep, Id::PowRestart, Id::PowShut};
        float w3 = (x1 - x0 - 2 * kS2 - 2 * kS1) / 3;
        for (int i = 0; i < 3; i++) {
            float l = x0 + kS2 + i * (w3 + kS1);
            add(ids[i], {l, y + kS2, l + w3, y + kPowH - kS2}, 0, kRadiusSmall);
        }
        y += kPowH + kGap;
    }
    y += kInset - kGap;
    if (y > MaxH() && listed > 1) {  // a long list would push the panel off the screen: list only what fits
        listCap = listed - std::min<size_t>(listed - 1, static_cast<size_t>(std::ceil((y - MaxH()) / kListRowH)));
        Layout();
        listCap = SIZE_MAX;
        return;
    }
    height = std::min({y, MaxH()});
}

// The panel follows `height`: the frame (pill, glass, clip, rim, shadow) springs to it, growing upward from
// the fixed bottom edge; the window never moves or resizes, so nothing lands a frame apart.
void ControlCenter::Impl::ApplySize() {
    const float hPx = std::ceil(height * s);
    if (hPx == shownPx) return;
    rect.top = rect.bottom - static_cast<LONG>(hPx);
    Resize(hPx, st == St::Open && !settling);
    if (st == St::Open) {
        POINT p;  // the panel moved under a still pointer: hover what is there now
        if (GetCursorPos(&p) && pressed.id == Id::None && drag == Id::None) {
            auto* it = HitAt((p.x - rect.left) / s, (p.y - rect.top) / s);
            hover = it ? *it : Item{};
            SetHover(it);
            HoverChip(ChipOf(it));
        }
        if (api->onResized) api->onResized();
    }
}

void ControlCenter::Impl::Resize(float hPx, bool animate) {
    const float wPx = std::ceil(kW * s), oldPx = shownPx;
    shownPx = hPx;
    const wfn::float3 at{std::lround(motion::kPad * s) * 1.0f, TopPx(), 0};
    if (!animate) {
        pill.StopAnimation(L"Offset");
        pill.Offset(at);
    } else {
        auto spring = [&] {
            auto a = comp.CreateSpringVector2Animation();
            a.DampingRatio(0.86f);
            a.Period(std::chrono::milliseconds(60));
            return a;
        };
        auto move = comp.CreateSpringVector3Animation();
        move.FinalValue(winrt::Windows::Foundation::IReference<wfn::float3>(at));
        move.DampingRatio(0.86f);
        move.Period(std::chrono::milliseconds(60));
        pill.StartAnimation(L"Offset", move);
        auto size = [&](wuc::CompositionObject const& o, wfn::float2 to) {
            auto a = spring();
            a.FinalValue(winrt::Windows::Foundation::IReference<wfn::float2>(to));
            o.StartAnimation(L"Size", a);
        };
        size(pill, {wPx, hPx});
        size(inner, {wPx, hPx});
        if (shadowV) size(shadowV, {wPx, hPx});
        size(clip, {wPx, hPx});
        size(rim, {wPx - s, hPx - s});
        if (specGeo) size(specGeo, {wPx - 3 * s, hPx - 3 * s});
        if (ghost) { inner.Children().Remove(ghost); ghost = nullptr; }
        const float S = std::round(split * s), maxPx = MaxPx();
        // `inner` is springing to hPx: `lower` keeps its drawing on the bottom edge and starts where `content` ends.
        auto follow = [&](wuc::CompositionObject const& o, wchar_t const* prop, wchar_t const* expr, float h) {
            auto e = comp.CreateExpressionAnimation(expr);
            e.SetReferenceParameter(L"inner", inner);
            e.SetScalarParameter(L"H", h);
            e.SetScalarParameter(L"S", S);
            o.StartAnimation(prop, e);
        };
        upperClip.BottomInset(maxPx - S);
        follow(lower, L"Offset.Y", L"inner.Size.Y - H", hPx);
        follow(lowerClip, L"TopInset", L"S + Max(0, H - inner.Size.Y)", hPx);
        if (oldPx > hPx) {
            // Closing: the old drawing of what went away stays put on the bottom edge and is covered from the top.
            auto old = comp.CreateSpriteVisual();
            old.Size(content.Size());
            old.Brush(content.Brush());  // the old drawing keeps its surface; the new layout gets a fresh one
            auto oc = comp.CreateInsetClip();
            oc.BottomInset(maxPx - (S + oldPx - hPx));
            old.Clip(oc);
            follow(old, L"Offset.Y", L"inner.Size.Y - H", oldPx);
            follow(oc, L"TopInset", L"S + Max(0, H - inner.Size.Y)", oldPx);
            auto fade = comp.CreateScalarKeyFrameAnimation();
            fade.InsertKeyFrame(1.0f, 0.0f, comp.CreateCubicBezierEasingFunction({0.4f, 0.0f}, {1.0f, 1.0f}));
            fade.Duration(std::chrono::milliseconds(ui::kSlowMs));
            old.StartAnimation(L"Opacity", fade);
            inner.Children().InsertBelow(old, content);
            ghost = old;
        }
        if (oldPx > 0) {
            surf = gdev.CreateDrawingSurface({wPx, maxPx}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                             winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            auto sb = comp.CreateSurfaceBrush(surf);
            sb.Stretch(wuc::CompositionStretch::None);
            sb.HorizontalAlignmentRatio(0);
            sb.VerticalAlignmentRatio(0);
            content.Brush(sb);
            lower.Brush(sb);
        }
        return;
    }
    // A spring left running by a close in mid-drawer would keep pulling these to the old height and win over
    // the plain sets below, so the reopened frame would not match its content: stop them first.
    pill.StopAnimation(L"Size");
    inner.StopAnimation(L"Size");
    clip.StopAnimation(L"Size");
    rim.StopAnimation(L"Size");
    if (shadowV) shadowV.StopAnimation(L"Size");
    if (specGeo) specGeo.StopAnimation(L"Size");
    if (ghost) { inner.Children().Remove(ghost); ghost = nullptr; }
    pill.Size({wPx, hPx});
    inner.Size({wPx, hPx});
    if (shadowV) shadowV.Size({wPx, hPx});
    clip.Size({wPx, hPx});
    rim.Size({wPx - s, hPx - s});
    if (specGeo) specGeo.Size({wPx - 3 * s, hPx - 3 * s});
    if (upperClip) upperClip.BottomInset(0);
    if (lower) {
        lower.StopAnimation(L"Offset.Y");
        lower.Offset({0, 0, 0});
        lowerClip.StopAnimation(L"TopInset");
        lowerClip.TopInset(MaxPx());
    }
}

void ControlCenter::Impl::Settle() {
    if (!settling) return;
    settling = false;
    if (hwnd) KillTimer(hwnd, kTimerSettle);
    if (std::exchange(needSync, false)) { SyncAll(); held = true; }
    if (std::exchange(held, false)) Render();
    Snapshot();  // test hook: once the panel shows its settled content
}

void ControlCenter::Impl::Render() {
    if (!surf || st != St::Open) return;
    if (settling) { held = true; return; }
    const auto before = items;
    Layout();
    split = 0;  // the lowest edge of what kept its place from the top (what changed is under it)
    for (auto const& it : items)
        for (auto const& b : before)
            if (Same(b, it) && b.r.top == it.r.top && b.r.bottom == it.r.bottom) split = std::max(split, it.r.bottom);
    ApplySize();
    auto interop = surf.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT off{};
    if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return;
    try {
        Paint(dc.get(), static_cast<float>(off.x), static_cast<float>(off.y), nullptr, nullptr);
    } catch (...) {}
    interop->EndDraw();
    if (drag == Id::None) SyncChips();  // a slider drag changes no chip: don't redraw 17 of them per move
}

// Draws every item (or just `only`, minus `skip`) with the panel origin at (tx, ty) device px.
void ControlCenter::Impl::Paint(ID2D1DeviceContext* dc, float tx, float ty, Item const* only, Item const* skip) {
    using namespace ui;
    dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(tx, ty));
    dc->Clear(ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    winrt::com_ptr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(kText, br.put());
    const bool darkTheme = theme::Dark();
    const D2D1_COLOR_F kFill = darkTheme ? ColorF(1, 1, 1, 0.94f) : ColorF(1, 1, 1, 1.0f),
                       kOnFill = darkTheme ? ColorF(0x1C1C1E) : ColorF(0.11f, 0.11f, 0.12f, 0.62f);

    auto box = [&](D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
        br->SetColor(c);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br.get());
    };
    // A module: one flat fill on the glass. No border and no shading: the glass is the only surface with an
    // edge, so modules read as parts of one panel instead of boxes in a box.
    auto tile = [&](D2D1_RECT_F r) {
        box(r, kRadiusLarge, theme::P().fill);
        if (!darkTheme) {
            br->SetColor(theme::P().line);
            dc->DrawRoundedRectangle(D2D1::RoundedRect({r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f}, kRadiusLarge, kRadiusLarge), br.get(), 1.0f);
        }
    };
    auto txt = [&](std::wstring const& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
        br->SetColor(c);
        dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, r, br.get());
    };
    auto glyph = [&](wchar_t g, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) { txt(std::wstring(1, g), f, r, c); };
    // A round control: accent when on, a quiet disc when off; a glyph, or the half-filled disc for dark mode.
    auto round = [&](float cx, float cy, float rad, bool on, bool avail, wchar_t g, IDWriteTextFormat* f, bool darkMark) {
        const float a = avail ? 1.0f : 0.4f;
        br->SetColor(theme::A(on ? kAccent : kOff, a));
        dc->FillEllipse(D2D1::Ellipse({cx, cy}, rad, rad), br.get());
        const D2D1_COLOR_F ink = theme::A(on ? kOnAccent : kText, a);
        if (darkMark) {
            float k = rad * 0.36f;
            br->SetColor(ink);
            dc->DrawEllipse(D2D1::Ellipse({cx, cy}, k, k), br.get(), 1.5f);
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
            glyph(g, f, {cx - rad, cy - rad, cx + rad, cy + rad}, ink);
        }
    };
    // Two lines: a title over a muted status, left-aligned in r and centred vertically.
    auto twoLines = [&](std::wstring const& title, std::wstring const& sub, D2D1_RECT_F r, float a) {
        const float cy = (r.top + r.bottom) / 2;
        txt(title, fTitle.get(), {r.left, cy - kTitleLine + 1, r.right, cy + 1}, theme::A(kText, a));
        txt(sub, fSub.get(), {r.left, cy + 1, r.right, cy + kCaptionLine + 1}, theme::A(kDim, a));
    };
    // A toggle pill (Do not disturb, Night light, Dark mode): the round icon at the left, the name over its state.
    auto togglePill = [&](D2D1_RECT_F r, bool on, wchar_t g, bool darkMark, std::wstring const& title) {
        tile(r);
        const float icx = r.left + kS3 + kRound / 2, icy = (r.top + r.bottom) / 2;
        round(icx, icy, kRound / 2, on, true, g, fIcon.get(), darkMark);
        twoLines(title, on ? L"On" : L"Off", {icx + kRound / 2 + kS3, r.top, r.right - kS2, r.bottom}, 1.0f);
    };
    // An icon-only tile (Airplane mode, Hotspot): the round toggle centred on a tile.
    auto iconTile = [&](D2D1_RECT_F r, bool on, bool avail, wchar_t g) {
        tile(r);
        round((r.left + r.right) / 2, (r.top + r.bottom) / 2, kRound / 2, on, avail, g, fIcon.get(), false);
    };
    for (auto const& it : items) {
        if (only && !Same(it, *only)) continue;
        if (skip && Same(it, *skip)) continue;
        if (!only && chipLayer && Scalable(it.id)) continue;  // drawn on its chip
        auto const& r = it.r;
        const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        switch (it.id) {
            case Id::WifiPill: case Id::BtPill: case Id::RowTile: case Id::DisplayTile: case Id::ListTile: case Id::MediaTile: case Id::PowerTile:
                tile(r);
                if (it.id == Id::DisplayTile && bright >= 0)
                    txt(L"Display", fTitle.get(), {r.left + kS4, r.top + kS4, r.right - kS4, r.top + kS4 + kTitleLine}, kText);
                if (it.id == Id::ListTile)
                    txt(expand == Expand::Wifi ? L"Wi-Fi networks" : L"Bluetooth devices", fTitle.get(),
                        {r.left + kS4, r.top + kS4, r.right - kS4, r.top + kS4 + kTitleLine}, kText);
                break;
            case Id::SoundTile:
                txt(L"Sound", fTitle.get(), {r.left + kS4, r.top, r.left + 112, r.bottom}, theme::A(kText, speaker.Available() ? 1.0f : 0.5f));
                break;
            case Id::Clock: {
                wchar_t tb[32] = L"", db[64] = L"";
                SYSTEMTIME now;
                GetLocalTime(&now);
                lastMin = now.wMinute;
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS | TIME_NOTIMEMARKER, &now, nullptr, tb, 32);
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"dddd, d MMMM", db, 64, nullptr);
                txt(tb, fTime.get(), {r.left + kS1, r.top, r.right, r.top + 36}, kText);
                txt(db, fSub.get(), {r.left + kS1, r.top + 36, r.right, r.bottom - kS1}, kDim);
                break;
            }
            case Id::Battery: {
                box(r, kRadiusSmall, theme::P().fill);
                wchar_t g = 0xE83E;  // plug
                if (battery.hasBattery) g = static_cast<wchar_t>((battery.ac ? 0xEBAB : 0xEBA0) + std::min(10, battery.percent / 10));
                glyph(g, fIcon.get(), {r.left + kS3, r.top, r.left + kS3 + 20, r.bottom}, kText);
                std::wstring cap = battery.ac ? L"Plugged in" : L"Battery";
                switch (mode) {
                    case svc::PowerMode::Efficiency: cap = L"Efficiency"; break;
                    case svc::PowerMode::Balanced: cap = L"Balanced"; break;
                    case svc::PowerMode::Performance: case svc::PowerMode::Max: cap = L"Performance"; break;
                    default: break;
                }
                const float x = r.left + kS3 + 20 + kS2;
                if (battery.hasBattery) twoLines(std::to_wstring(battery.percent) + L"%", cap, {x, r.top, r.right - kS2, r.bottom}, 1.0f);
                else txt(cap, fTitle.get(), {x, r.top, r.right - kS2, r.bottom}, kText);
                break;
            }
            case Id::WifiText: case Id::BtText: {
                bool wifi = it.id == Id::WifiText;
                bool avail = wifi ? radios.HasWifi() : radios.HasBluetooth(), on = wifi ? wifiOn : btOn;
                std::wstring sub;
                if (wifi) sub = !avail ? L"Unavailable" : !on ? L"Off" : ssid.empty() ? L"Not connected" : ssid;
                else {
                    sub = !avail ? L"Unavailable" : on ? L"On" : L"Off";
                    if (on) for (auto const& d : svc::BtDevices()) if (d.connected) { sub = d.name; break; }
                }
                twoLines(wifi ? L"Wi-Fi" : L"Bluetooth", sub, {r.left, r.top, r.right - 20, r.bottom}, avail ? 1.0f : 0.5f);
                bool open = expand == (wifi ? Expand::Wifi : Expand::Bluetooth);
                glyph(open ? 0xE70E : 0xE76C, fIconS.get(), {r.right - 20, cy - 10, r.right, cy + 10}, kDim);
                break;
            }
            case Id::Label: {
                const bool air = it.arg == 0;
                const bool avail = air ? (radios.HasWifi() || radios.HasBluetooth()) : hotspotAvail;
                const bool on = air ? (avail && !wifiOn && !btOn) : hotspotOn;
                twoLines(air ? L"Airplane mode" : L"Hotspot", !avail ? L"Unavailable" : on ? L"On" : L"Off", r, avail ? 1.0f : 0.5f);
                break;
            }
            case Id::Wifi: round(cx, cy, kRound / 2, wifiOn, radios.HasWifi(), 0xE701, fIcon.get(), false); break;
            case Id::Bluetooth: round(cx, cy, kRound / 2, btOn, radios.HasBluetooth(), 0xE702, fIcon.get(), false); break;
            case Id::Airplane: {
                bool avail = radios.HasWifi() || radios.HasBluetooth();
                iconTile(r, avail && !wifiOn && !btOn, avail, 0xE709);
                break;
            }
            case Id::Hotspot: iconTile(r, hotspotOn, hotspotAvail, 0xE704); break;
            case Id::Dnd: togglePill(r, dnd, 0xE708, false, L"Do not disturb"); break;
            case Id::Night: togglePill(r, night, 0xF08C, false, L"Night light"); break;
            case Id::Dark: togglePill(r, dark, 0, true, L"Dark mode"); break;
            case Id::Snip: round(cx, cy, kRoundAction / 2, false, true, 0xE8C6, fIcon.get(), false); break;
            case Id::Cast: round(cx, cy, kRoundAction / 2, false, true, 0xE7F4, fIcon.get(), false); break;
            case Id::Settings: round(cx, cy, kRoundAction / 2, false, true, 0xE713, fIcon.get(), false); break;
            case Id::Lock: round(cx, cy, kRoundAction / 2, false, true, 0xE72E, fIcon.get(), false); break;
            case Id::Power: round(cx, cy, kRoundAction / 2, powerOpen, true, 0xE7E8, fIcon.get(), false); break;
            case Id::Mic: round(cx, cy, kRoundAction / 2, micMuted, mic.Available(), micMuted ? 0xEC54 : 0xE720, fIcon.get(), false); break;
            case Id::PowSleep: case Id::PowRestart: case Id::PowShut:
                box(r, kRadiusSmall, theme::P().fill);
                txt(it.id == Id::PowSleep ? L"Sleep" : it.id == Id::PowRestart ? L"Restart" : L"Shut down", fBtn.get(), r, kText);
                break;
            case Id::Bright: case Id::Vol: {  // a thick track; the fill carries the icon at its left end
                float h = r.bottom - r.top, f = it.id == Id::Vol ? volume : bright / 100.0f;
                if (it.id == Id::Vol && !speaker.Available()) { box(r, h / 2, theme::A(kOff, 0.5f)); break; }
                box(r, h / 2, kOff);
                D2D1_RECT_F fr{r.left, r.top, r.left + h + f * (r.right - r.left - h), r.bottom};
                box(fr, h / 2, kFill);
                wchar_t g = it.id == Id::Bright ? 0xE706 : (muted || volume <= 0.001f ? 0xE74F : 0xE767);
                glyph(g, fIconS.get(), {r.left, r.top, r.left + h, r.bottom}, kOnFill);
                break;
            }
            case Id::OutRow: {
                std::wstring name = L"Output";
                for (auto const& d : outs) if (d.isDefault) name = d.name;
                if (auto p = name.find(L" ("); p != std::wstring::npos && p > 0) name.resize(p);  // "Speakers (Realtek…)" -> "Speakers"
                txt(name, fSubR.get(), {r.left, r.top, r.right - 20, r.bottom}, kDim);
                glyph(outsOpen ? 0xE70E : 0xE70D, fIconS.get(), {r.right - 20, r.top, r.right, r.bottom}, kDim);
                break;
            }
            case Id::OutDev: case Id::WifiNet: case Id::BtDev: {
                std::wstring name, sub;
                bool mark = false;
                if (it.id == Id::OutDev) { name = outs[it.arg].name; mark = outs[it.arg].isDefault; }
                else if (it.id == Id::WifiNet) {
                    auto const& n = svc::WifiNets()[it.arg];
                    name = n.ssid; mark = n.connected;
                    sub = n.connected ? L"Connected" : L"";
                } else {
                    auto const& d = svc::BtDevices()[it.arg];
                    name = d.name; mark = d.connected; sub = d.connected ? L"Connected" : L"";
                }
                txt(name, fSub.get(), {r.left + kS2, r.top, r.right - (sub.empty() ? 32 : 112), r.bottom}, mark ? kText : kDim);
                if (!sub.empty()) txt(sub, fSubR.get(), {r.right - 104, r.top, r.right - 32, r.bottom}, kDim);
                if (mark) glyph(0xE73E, fIconS.get(), {r.right - 28, r.top, r.right - kS2, r.bottom}, kAccent);
                break;
            }
            case Id::Prev: case Id::PlayPause: case Id::Next: {
                auto const& ms = media.State();
                if (!ms.has) break;  // nothing to control: no controls
                bool ok = it.id == Id::Prev ? ms.canPrev : it.id == Id::Next ? ms.canNext : true;
                wchar_t g = it.id == Id::Prev ? 0xE892 : it.id == Id::Next ? 0xE893 : (ms.playing ? 0xE769 : 0xE768);
                glyph(g, it.id == Id::PlayPause ? fIcon.get() : fIconS.get(), r, theme::A(kText, ok ? 1.0f : 0.3f));
                break;
            }
            default: break;
        }
        if (it.id == Id::MediaTile) {
            auto const& ms = media.State();
            const float art = 48;
            D2D1_RECT_F ar{r.left + kS4, cy - art / 2, r.left + kS4 + art, cy + art / 2};
            if (ms.artSize > 0 && ms.art.size() == static_cast<size_t>(ms.artSize) * ms.artSize) {
                winrt::com_ptr<ID2D1Bitmap> bmp;
                auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                winrt::com_ptr<ID2D1BitmapBrush> bb;
                if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(ms.artSize, ms.artSize), ms.art.data(), ms.artSize * 4, &props, bmp.put())) &&
                    SUCCEEDED(dc->CreateBitmapBrush(bmp.get(), bb.put()))) {
                    float k = art / ms.artSize;
                    bb->SetTransform(D2D1::Matrix3x2F::Scale(k, k) * D2D1::Matrix3x2F::Translation(ar.left, ar.top));
                    bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(ar, kRadiusSmall, kRadiusSmall), bb.get());
                }
            } else {
                box(ar, kRadiusSmall, kOff);
                glyph(0xE8D6, fIcon.get(), ar, kDim);  // music note
            }
            const float tl = ar.right + kS3, tr = ms.has ? r.right - kS3 - 3 * kRound - 2 * kS1 - kS2 : r.right - kS3;
            if (ms.has) twoLines(ms.title.empty() ? L"Now Playing" : ms.title, ms.artist, {tl, r.top, tr, r.bottom}, 1.0f);
            else twoLines(L"Not Playing", L"Media you play shows up here", {tl, r.top, tr, r.bottom}, 1.0f);
        }
    }
}

// HALO_CC_SNAPSHOT=<file.png>: once each open has settled, the panel's drawing (chips included) over a flat stand-in
// for the glass, written as a PNG. For design checks that do not depend on the screen (a locked
// session shows nothing to a screen capture).
void ControlCenter::Impl::Snapshot() {
    wchar_t path[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"HALO_CC_SNAPSHOT", path, MAX_PATH)) return;
    const UINT w = static_cast<UINT>(std::ceil(kW * s)), h = static_cast<UINT>(std::ceil(height * s));
    winrt::com_ptr<IWICImagingFactory> wic;
    winrt::com_ptr<IWICBitmap> bmp;
    winrt::com_ptr<ID2D1Factory> fac;
    winrt::com_ptr<ID2D1RenderTarget> rt;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), wic.put_void())) ||
        FAILED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.put())) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, fac.put())) ||
        FAILED(fac->CreateWicBitmapRenderTarget(bmp.get(), D2D1::RenderTargetProperties(), rt.put())))
        return;
    auto dc = rt.try_as<ID2D1DeviceContext>();
    if (!dc) return;
    // The stand-in glass: the tint over a mid-dark backdrop, inside the panel's rounded shape.
    const auto t = theme::Glass(cfg()).tint;
    const float k = t.a / 255.0f, back = theme::Dark() ? 0.22f : 0.75f;
    auto mix = [&](uint8_t c) { return c / 255.0f * k + back * (1 - k); };
    auto layer = std::exchange(chipLayer, nullptr);  // so Paint draws the chips too
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    winrt::com_ptr<ID2D1SolidColorBrush> glass;
    dc->CreateSolidColorBrush(D2D1::ColorF(mix(t.r), mix(t.g), mix(t.b), 1), glass.put());
    dc->FillRoundedRectangle(D2D1::RoundedRect({0, 0, static_cast<float>(w), static_cast<float>(h)}, kRadius * s, kRadius * s), glass.get());
    winrt::com_ptr<ID2D1BitmapRenderTarget> layerRt;
    // Paint clears its target, so it draws into a layer that is then composed over the glass.
    if (SUCCEEDED(dc->CreateCompatibleRenderTarget(D2D1::SizeF(static_cast<float>(w), static_cast<float>(h)), layerRt.put()))) {
        if (auto ldc = layerRt.try_as<ID2D1DeviceContext>()) {
            ldc->BeginDraw();
            try { Paint(ldc.get(), 0, 0, nullptr, nullptr); } catch (...) {}
            ldc->EndDraw();
            winrt::com_ptr<ID2D1Bitmap> painted;
            layerRt->GetBitmap(painted.put());
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->DrawBitmap(painted.get());
        }
    }
    dc->EndDraw();
    chipLayer = layer;
    winrt::com_ptr<IWICStream> stream;
    winrt::com_ptr<IWICBitmapEncoder> enc;
    winrt::com_ptr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateStream(stream.put())) || FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put())) ||
        FAILED(enc->Initialize(stream.get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(frame.put(), nullptr)) ||
        FAILED(frame->Initialize(nullptr)))
        return;
    frame->WriteSource(bmp.get(), nullptr);
    frame->Commit();
    enc->Commit();
    svc::Log(std::wstring(L"snapshot -> ") + path);
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
    mk(fTitle, ui::kFontText, SB, ui::kBody, L, M);
    mk(fSub, ui::kFontText, R, ui::kCaption, L, M);
    mk(fSubR, ui::kFontText, R, ui::kCaption, T, M);
    mk(fBatt, ui::kFontText, SB, ui::kBody, L, M);
    mk(fIcon, ui::kFontIcons, R, ui::kIcon, C, M);
    mk(fIconS, ui::kFontIcons, R, ui::kIconSmall, C, M);
    mk(fCap, ui::kFontText, R, ui::kCaption, C, M);
    mk(fTime, ui::kFontDisplay, SB, ui::kDisplay, L, M);
    mk(fDate, ui::kFontText, R, ui::kCaption, L, N);
    mk(fBtn, ui::kFontText, SB, ui::kBody, C, M);
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
    pill.Offset({pad, TopPx(), 0});
    root.Children().InsertAtTop(pill);
    // The shadow sits beside the clipped panel (a clip would cut it off), both riding the pill's motion.
    if (auto sh = MakeShadow(comp, gdev, wPx, hPx, kRadius * s, theme::P().shadowBlur * s, theme::P().shadowY * s, theme::P().shadow)) {
        shadowV = sh;
        pill.Children().InsertAtTop(sh);
    }
    inner = comp.CreateContainerVisual();
    inner.Size({wPx, hPx});
    pill.Children().InsertAtTop(inner);

    auto glass = comp.CreateSpriteVisual();
    glass.RelativeSizeAdjustment({1, 1});
    // The host backdrop already comes blurred by DWM; another Gaussian pass on top cost dropped frames
    // whenever the panel moved (measured with tools/framegaps.cpp) for a barely softer look.
    glass.Brush(comp.CreateHostBackdropBrush());
    inner.Children().InsertAtTop(glass);

    auto tint = comp.CreateSpriteVisual();
    tint.RelativeSizeAdjustment({1, 1});
    const auto glassTint = theme::Glass(cfg()).tint;
    tint.Brush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(glassTint.a, glassTint.r, glassTint.g, glassTint.b)));
    inner.Children().InsertAtTop(tint);

    try {
        auto noise = comp.CreateSpriteVisual();
        noise.RelativeSizeAdjustment({1, 1});
        noise.Opacity(theme::P().noise);
        if (!noiseBrush || noiseW != static_cast<int>(wPx)) {  // generated once per scale, then shared by every open
            noiseBrush = NoiseBrush(static_cast<int>(wPx), static_cast<int>(std::ceil(kMaxH * s)));
            noiseW = static_cast<int>(wPx);
        }
        noise.Brush(noiseBrush);
        inner.Children().InsertAtTop(noise);
    } catch (...) {}

    surf = gdev.CreateDrawingSurface({wPx, MaxPx()}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                     winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    content = comp.CreateSpriteVisual();
    content.Size({wPx, MaxPx()});
    auto sb = comp.CreateSurfaceBrush(surf);
    sb.Stretch(wuc::CompositionStretch::None);
    content.Brush(sb);
    inner.Children().InsertAtTop(content);
    upperClip = comp.CreateInsetClip();
    content.Clip(upperClip);
    lower = comp.CreateSpriteVisual();
    lower.Size({wPx, MaxPx()});
    lower.Brush(sb);
    lowerClip = comp.CreateInsetClip();
    lowerClip.TopInset(MaxPx());  // nothing until a height change splits the drawing
    lower.Clip(lowerClip);
    inner.Children().InsertAbove(lower, content);

    // The hover highlight: one rounded rect that fades and glides between controls.
    hv = comp.CreateShapeVisual();
    hv.RelativeSizeAdjustment({1, 1});
    hv.Opacity(0);
    hvGeo = comp.CreateRoundedRectangleGeometry();
    auto hs = comp.CreateSpriteShape(hvGeo);
    hs.FillBrush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(theme::Dark() ? 0x26 : 0x12, theme::Dark() ? 0xFF : 0x00, theme::Dark() ? 0xFF : 0x00, theme::Dark() ? 0xFF : 0x00)));
    hv.Shapes().Append(hs);
    inner.Children().InsertAtTop(hv);
    hvShown = false;

    fx = comp.CreateContainerVisual();
    fx.RelativeSizeAdjustment({1, 1});
    inner.Children().InsertAtTop(fx);

    const float t = s;  // 1 px rim, inset by half its width so it stays inside the clip
    rim = comp.CreateRoundedRectangleGeometry();
    rim.Offset({t / 2, t / 2});
    rim.CornerRadius({kRadius * s - t / 2, kRadius * s - t / 2});
    rim.Size({wPx - t, hPx - t});
    auto gradient = comp.CreateLinearGradientBrush();
    gradient.MappingMode(wuc::CompositionMappingMode::Relative);
    gradient.StartPoint({0, 0});
    gradient.EndPoint({0, 1});
    auto stop = [&](float at, theme::Argb c) { gradient.ColorStops().Append(comp.CreateColorGradientStop(at, theme::C(c))); };
    stop(0.0f, theme::P().rimTop); stop(0.35f, theme::P().rimMid); stop(0.7f, theme::P().rimLow); stop(1.0f, theme::P().rimBottom);
    auto sprite = comp.CreateSpriteShape(rim);
    sprite.StrokeBrush(gradient);
    sprite.StrokeThickness(t);
    auto rimVisual = comp.CreateShapeVisual();
    rimVisual.RelativeSizeAdjustment({1, 1});
    rimVisual.Shapes().Append(sprite);
    if (theme::P().edge.a) {  // the 1px hairline that separates light glass from a light wallpaper
        auto edge = comp.CreateSpriteShape(rim);
        edge.StrokeBrush(comp.CreateColorBrush(theme::C(theme::P().edge)));
        edge.StrokeThickness(t);
        rimVisual.Shapes().Append(edge);
    }
    // A specular line just inside the rim along the top: light catching the glass's thickness. Static.
    auto spec = comp.CreateRoundedRectangleGeometry();
    spec.Offset({t * 1.5f, t * 1.5f});
    spec.CornerRadius({kRadius * s - t * 1.5f, kRadius * s - t * 1.5f});
    spec.Size({wPx - 3 * t, hPx - 3 * t});
    auto sheen = comp.CreateLinearGradientBrush();
    sheen.MappingMode(wuc::CompositionMappingMode::Absolute);
    sheen.StartPoint({0, 0});
    sheen.EndPoint({0, 72 * s});  // fixed depth, so it does not stretch with the panel's height
    sheen.ColorStops().Append(comp.CreateColorGradientStop(0.0f, winrt::Windows::UI::ColorHelper::FromArgb(theme::Dark() ? 0x38 : 0x80, 0xFF, 0xFF, 0xFF)));
    sheen.ColorStops().Append(comp.CreateColorGradientStop(1.0f, winrt::Windows::UI::ColorHelper::FromArgb(0, 0xFF, 0xFF, 0xFF)));
    auto specShape = comp.CreateSpriteShape(spec);
    specShape.StrokeBrush(sheen);
    specShape.StrokeThickness(t);
    rimVisual.Shapes().Append(specShape);
    specGeo = spec;
    inner.Children().InsertAtTop(rimVisual);

    chipLayer = comp.CreateContainerVisual();
    chipLayer.RelativeSizeAdjustment({1, 1});
    pill.Children().InsertAtTop(chipLayer);
    ptr = comp.CreatePropertySet();
    ptr.InsertVector2(L"P", {-1000, -1000});

    clip = comp.CreateRoundedRectangleGeometry();
    clip.CornerRadius({kRadius * s, kRadius * s});
    clip.Size({wPx, hPx});
    inner.Clip(comp.CreateGeometricClip(clip));
}

// ---------------------------------------------------------------------------------------------
// public

ControlCenter::ControlCenter(HINSTANCE inst, HWND owner, wuc::Compositor comp, wuc::CompositionGraphicsDevice gdev)
    : m(std::make_unique<Impl>(this, inst, owner, std::move(comp), std::move(gdev))) {
    m->RegisterClasses();
    // Whether the display has a brightness control is learned off-thread; learn it now, not on the first
    // open, or the Display slider turns up mid-open and the panel grows under the pointer.
    svc::brightness::Probe(m->hot, 0);
    // Now Playing is followed all the time (GSMTC only calls back on changes), so the media tile is
    // already right when the panel opens instead of arriving a moment later.
    m->media.Start(m->hot, kMsgMedia, [p = m.get()] { if (p->st == Impl::St::Open) p->Render(); });
    // The same for the radios and the volume: started once, so an open never shows "Unavailable" or a
    // missing Sound row that then arrives and grows the panel (it read as the panel opening twice).
    m->radios.Start(m->hot, kMsgRadios);
    m->speaker.Bind(eRender, m->hot, kMsgAudio);
    m->mic.Bind(eCapture, m->hot, kMsgMic);
    m->SyncSpeaker();
    m->SyncMic();
}

ControlCenter::~ControlCenter() {
    m->media.Stop();
    m->radios.Stop();
    m->speaker.Unbind();
    m->mic.Unbind();
    try { m->Teardown(); } catch (...) {}  // a destructor is noexcept: a WinRT failure here would terminate the exit
    if (m->hot) DestroyWindow(m->hot);
}

// The glass or the layout toggles changed: a closed panel is torn down now, an open one closes and rebuilds on its next open.
void ControlCenter::OnSettingsChanged() {
    m->stale = true;
    if (m->st == Impl::St::Open) m->Close();
    else if (m->st == Impl::St::Closed && m->hwnd) m->Teardown();
}
void ControlCenter::Toggle() { IsOpen() ? Close() : Open(); }
void ControlCenter::Open(bool byPointer) { m->Open(byPointer); }
void ControlCenter::Close() { m->Close(); }
bool ControlCenter::IsOpen() const { return m->st == Impl::St::Open; }
// The panel itself (physical px), not the padded window.
RECT ControlCenter::PanelRectScreen() const { return m->st == Impl::St::Closed ? RECT{} : m->rect; }
HWND ControlCenter::Hwnd() const { return m->st == Impl::St::Closed ? nullptr : m->hwnd; }

}  // namespace halo
