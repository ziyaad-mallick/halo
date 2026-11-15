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

// Geometry, all from tokens.h: a 360 px panel, a 16 px inset, 16 px between groups and 8 px inside one,
// concentric corners. The window is the panel plus motion::kPad of empty margin on every side, so the
// spring's overshoot never clips; everything below is in panel coordinates (logical px).
constexpr float kW = ui::kPanelW, kInset = ui::kPanelInset, kRadius = ui::kPanelRadius;
constexpr float kHeaderH = ui::kRowH, kTitleLine = ui::kS4 + ui::kS1, kCaptionLine = ui::kS4, kPowH = ui::kRoundAction + ui::kS2;
constexpr float kMaxH = 900, kMargin = ui::kS2, kActW = 4 * ui::kS6;  // the width of a text button on a details page
// The home view at its full (non-dense) height: the details page and the "does it fit" test measure against it.
// Five group gaps between the header, the two connectivity pills, two rows of minis, two sliders, the media card and the footer.
constexpr float kHomeH = 2 * ui::kPanelInset + 5 * ui::kGroupGap + ui::kButton + ui::kTileH + 2 * ui::kRoundAction + ui::kInnerGap + 2 * ui::kButton + ui::kMediaH + ui::kButton;

constexpr UINT kMsgAudio = WM_APP + 1, kMsgMic = WM_APP + 2, kMsgRadios = WM_APP + 3, kMsgBright = WM_APP + 4,
               kMsgMedia = WM_APP + 5, kMsgWifi = WM_APP + 6, kMsgBt = WM_APP + 7, kMsgLate = WM_APP + 8,
               kMsgTestClick = WM_APP + 30;
constexpr UINT_PTR kTimerClose = 1, kTimerChord = 2, kTimerClock = 3, kTimerLeave = 4, kTimerSettle = 5;
// While the opening spring runs nothing redraws: updates that arrive (the deferred reads, radios, media,
// brightness) are held and drawn once, kSettleMs after the show. A redraw mid-motion cost a frame.
constexpr UINT kSettleMs = 240;
// Auto-hide: while open, where the pointer is gets checked every kLeaveTickMs; kLeaveMs outside the panel
// (plus kLeaveSlack px around it) closes it. Then the ring that springs out of a toggle (from kPulseFrom to
// kPulseTo of its size) and how far an arrow key moves a slider.
constexpr UINT kLeaveTickMs = 50, kLeaveMs = 200;
constexpr float kLeaveSlack = 18, kPulseFrom = 0.9f, kPulseTo = 1.5f, kSliderStep = 0.05f;
constexpr wchar_t kPanelClass[] = L"HaloControlCenter", kHotClass[] = L"HaloControlCenterTimers";

enum class Id {
    None, WifiPill, BtPill, WifiText, BtText, Wifi, Bluetooth, Airplane, Hotspot, Night, Dark, Dnd,
    DisplayTile, Bright, Vol, OutRow, OutDev, WifiNet, BtDev, MediaTile, Prev, PlayPause, Next,
    Snip, Cast, Settings, Battery, Lock, Power, Mic, PowerTile, PowSleep, PowRestart, PowShut, Clock, Back, DeviceListTile, DiscoveryAction, RadioSettings
};
// Passive items only hold a place (a tile, the sliders' area): the pointer and the keyboard skip them.
bool Passive(Id id) {
    return id == Id::WifiPill || id == Id::BtPill || id == Id::DisplayTile || id == Id::MediaTile || id == Id::PowerTile || id == Id::DeviceListTile;
}
enum class Expand { None, Wifi, Bluetooth };

struct Item {
    Id id = Id::None;
    D2D1_RECT_F r{};
    int arg = 0;
    float rad = ui::kRadiusHover;  // corner radius of the hover highlight
};
bool Same(Item const& a, Item const& b) { return a.id == b.id && a.arg == b.arg; }

bool In(D2D1_RECT_F const& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
winrt::Windows::UI::Color Col(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    return winrt::Windows::UI::ColorHelper::FromArgb(b(c.a), b(c.r), b(c.g), b(c.b));
}

// Ink and fills come from the shared palette. These refer into the live palette, so a theme or accent change
// from settings.ini shows at the next paint. kDim is the palette's secondary text (`muted`).
const D2D1_COLOR_F &kText = theme::detail::Cur().text, &kDim = theme::detail::Cur().muted, &kOff = theme::detail::Cur().off,
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
    wuc::CompositionRoundedRectangleGeometry clip{nullptr}, rim{nullptr}, hvGeo{nullptr};
    wuc::CompositionColorBrush hvBrush{nullptr};  // the highlight's fill: fillHover, or fillPress while pressed
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
    std::vector<svc::WifiNet> wifiRows;
    std::vector<svc::BtDevice> btRows;
    bool outsOpen = false;
    size_t outOffset = 0, outVisible = 0;
    size_t listOffset = 0, listVisible = 0;
    Expand expand = Expand::None;
    float volume = 0;
    bool muted = false, micMuted = false, dnd = false, wifiOn = false, btOn = false, dark = false, energy = false, night = false;
    bool hotspotAvail = false, hotspotOn = false, savedWifi = true, savedBt = true, btAsked = false, btDone = false;  // btDone: the paired-device read has answered
    int bright = -1;
    svc::Battery battery;
    svc::PowerMode mode = svc::PowerMode::Unknown;
    std::wstring ssid;
    std::vector<Item> items;
    Item hover, pressed;
    Item focus;             // the keyboard's control; drawn only once the keyboard has been used (kbFocus)
    bool kbFocus = false;
    // HALO_TEST + HALO_TEST_CC_MEDIA=1|0: a fixed Now Playing (or nothing playing) for design captures.
    svc::MediaState demoMedia;
    bool demoMediaOn = false;
    svc::MediaState const& Playing() const { return demoMediaOn ? demoMedia : media.State(); }
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
    void Paint(ID2D1DeviceContext* dc, float tx, float ty, Item const* only, Item const* skip, D2D1_RECT_F const* region = nullptr);
    void RepaintRegion(D2D1_RECT_F r);
    void PressFeedback(Item const* it);
    bool Enabled(Item const& it) const;
    void MoveFocus(int step);
    void MoveFocusDir(int dy);
    void NudgeSlider(int dir);
    void FocusItem(Item const& it);
    // Motion for the CC's own feedback honours the surface's speed and reduce_motion (which also turns every effect into a fade).
    std::chrono::milliseconds Fast(int ms) const {
        return settings::Get().reduceMotion ? 1ms : std::chrono::milliseconds(std::max(1, static_cast<int>(std::lround(ms / settings::Speed(cfg())))));
    }
    void BuildVisuals();
    wuc::CompositionBrush NoiseBrush(int w, int h);
    void Pulse(Item const& it);
    void SetHover(Item const* it);
    void Click(Item const& it);
    void ScrollDetails(int rows);
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
    bool Dense() const { return (mon.bottom - mon.top) / s < 636.0f; }
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
        case WM_DPICHANGED:
            // The composition tree and text formats use the DPI from Open(). Rebuild them on the next open;
            // keep a live frame from stretching stale surfaces across the new monitor scale.
            stale = true;
            if (st == St::Open) Close();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && st == St::Open) {  // clicked outside, unless it was the notification column
                HWND other = reinterpret_cast<HWND>(l);
                if (other && api->siblingHwnd && other == api->siblingHwnd()) return 0;
                svc::Log(L"panel deactivated, closing (focus went to " + std::to_wstring(reinterpret_cast<uintptr_t>(other)) + L")");
                Close();
            }
            return 0;
        case WM_KEYDOWN: {
            // Tab / Shift+Tab and Left / Right walk the controls; Up / Down walk them by row (on a details page they scroll
            // its list); Enter or Space presses the focused one; Left / Right on a slider move it. Esc goes back, then closes.
            const bool details = outsOpen || expand != Expand::None;
            if (w == VK_ESCAPE) {
                if (details) Click(Item{Id::Back});
                else Close();
            } else if (w == VK_TAB) {
                MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            } else if (w == VK_LEFT || w == VK_RIGHT) {
                if (focus.id == Id::Bright || focus.id == Id::Vol) NudgeSlider(w == VK_LEFT ? -1 : 1);
                else MoveFocus(w == VK_LEFT ? -1 : 1);
            } else if (w == VK_UP || w == VK_DOWN) {
                if (details) ScrollDetails(w == VK_UP ? -1 : 1);
                else MoveFocusDir(w == VK_UP ? -1 : 1);
            } else if ((w == VK_RETURN || w == VK_SPACE) && focus.id != Id::Bright && focus.id != Id::Vol) {
                for (auto const& it : items)
                    if (Same(it, focus) && Enabled(it)) { Item c = it; Click(c); break; }
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
            ScreenToClient(h, &p);
            const float px = p.x / s - motion::kPad, py = (p.y - TopPx()) / s;
            auto tile = std::find_if(items.begin(), items.end(), [](auto const& item) { return item.id == Id::DeviceListTile; });
            if (st == St::Open && tile != items.end() && In(tile->r, px, py)) {
                const int delta = GET_WHEEL_DELTA_WPARAM(w);
                if (delta) ScrollDetails((delta > 0 ? -1 : 1) * std::max(1, std::abs(delta) / WHEEL_DELTA));
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN:
            if (st != St::Open) return 0;
            Settle();  // a click shows its result at once
            if (x < 0 || y < 0 || x >= kW || y >= height) { Close(); return 0; }  // the transparent margin is "outside"
            if (kbFocus) { kbFocus = false; Render(); }  // the pointer takes over from the keyboard: the ring goes
            if (auto* it = HitAt(x, y)) {
                if (it->id == Id::Bright || it->id == Id::Vol) {
                    drag = it->id;
                    SetCapture(hwnd);
                    DragTo(x);
                } else if (Enabled(*it)) {
                    pressed = *it;
                    PressFeedback(it);  // the highlight answers at once, before anything else
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
            if (!Same(now, hover)) {
                hover = now;
                SetHover(it && Enabled(*it) ? it : nullptr);
            }
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
                PressFeedback(nullptr);
                if (it && Same(*it, p)) {
                    Item c = *it;  // Click re-lays-out and invalidates `it`
                    Click(c);
                }
            }
            return 0;
        case kMsgTestClick:  // HALO_TEST: wParam = an Id, clicked as if by the pointer
            if (st == St::Open && _wgetenv(L"HALO_TEST"))
                for (auto const& it : items)
                    if (static_cast<WPARAM>(it.id) == w) { Item c = it; Settle(); Click(c); Snapshot(); break; }
            return 0;
        case kMsgAudio:
            if (drag == Id::Vol) return 0;  // the pointer owns the value while it drags: the device's echo must not pull it back
            SyncSpeaker(); Render(); return 0;
        case kMsgMic: SyncMic(); Render(); return 0;
        case kMsgRadios:
            SyncRadios();
            if (st == St::Open && btOn && !btAsked) { btAsked = true; svc::BtRefresh(hot, kMsgBt); }
            else if (!btOn) { btAsked = false; svc::BtStop(); btRows.clear(); }
            Render();
            return 0;
        case kMsgBright:
            if (drag == Id::Bright) return 0;
            if (svc::brightness::Supported()) bright = svc::brightness::Level();
            Render();
            return 0;
        case kMsgBt:
            pressed = {};
            btDone = true;
            btRows = svc::BtDevices();
            Render();
            return 0;
        case kMsgWifi:
            pressed = {};
            wifiRows = svc::WifiNets();
            ssid = wifiOn ? svc::ConnectedSsid() : std::wstring();
            Render();
            return 0;
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

    HMONITOR hm = owner ? MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST)
                        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    mon = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s = dx / 96.0f;

    try {
        BuildFonts();  // first open only
        if (!demoMediaOn && _wgetenv(L"HALO_TEST") && _wgetenv(L"HALO_TEST_CC_MEDIA")) {
            demoMediaOn = true;
            demoMedia.has = _wgetenv(L"HALO_TEST_CC_MEDIA")[0] == L'1';
            demoMedia.playing = demoMedia.canPrev = demoMedia.canNext = true;
            demoMedia.title = L"Nightcall";
            demoMedia.artist = L"Kavinsky";
            demoMedia.artSize = 48;
            for (int y = 0; y < 48; ++y)
                for (int x = 0; x < 48; ++x) {
                    const float t = (x + y) / 94.0f;  // a diagonal gradient, #C4553A to #40264F
                    auto ch = [&](float a, float b) { return static_cast<uint32_t>(a + (b - a) * t); };
                    demoMedia.art.push_back(0xFF000000 | ch(0xC4, 0x40) << 16 | ch(0x55, 0x26) << 8 | ch(0x3A, 0x4F));
                }
        }
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
        rect = {mon.right - wPx - mg, mon.bottom - mg - hPx, 0, 0};  // beside the dock, within its work area
        rect.right = rect.left + wPx;
        rect.bottom = rect.top + hPx;
        SIZE wp = WindowPx();
        SetWindowPos(hwnd, HWND_TOPMOST, rect.left - pad, rect.bottom + pad - wp.cy, wp.cx, wp.cy, SWP_NOACTIVATE);
        shownPx = static_cast<float>(hPx);
        if (!pill) BuildVisuals();
        Resize(static_cast<float>(hPx), false);
        st = St::Open;
        if (btOn) { btAsked = true; svc::BtRefresh(hot, kMsgBt); }
        Render();
        motion::In(comp, pill, {pad * 1.0f, TopPx(), 0}, Travel(), cfg(), Corner());  // before the show: no frame at rest first
        // From the edge the panel only shows (the app under the pointer keeps the keyboard, and nothing
        // waits on a foreground change); a click in it activates it like any window.
        ShowWindow(hwnd, pointer ? SW_SHOWNOACTIVATE : SW_SHOW);
        settling = true;
        SetTimer(hwnd, kTimerSettle, static_cast<UINT>(kSettleMs / settings::Speed(cfg())), nullptr);
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
    KillTimer(hwnd, kTimerClock);  // the window is kept between opens: a live 15 s timer would wake the process forever
    // The scans stop; the radio and volume listeners stay, so the next open already shows the truth.
    svc::WifiStop();
    svc::BtStop();
    btAsked = false;
    wifiRows.clear();
    btRows.clear();
    SetHover(nullptr);
    hover = pressed = {};
    PressFeedback(nullptr);
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
    if (hvBrush) hvBrush.Color(Col(theme::P().fillHover));
    hvShown = false;
    focus = {};
    kbFocus = false;
    if (ghost) { inner.Children().Remove(ghost); ghost = nullptr; }
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
    } catch (winrt::hresult_error const&) {}
    shownPx = 0;
    ghost = lower = nullptr;
    upperClip = lowerClip = nullptr;
    surf = nullptr; content = nullptr; hv = nullptr; hvGeo = nullptr;
    hvBrush = nullptr; pill = nullptr; inner = nullptr; shadowV = nullptr; fx = nullptr; root = nullptr; target = nullptr; clip = nullptr; rim = nullptr;
    items.clear();
    rect = {};
    if (wasOpen && api->onHidden) api->onHidden();
}

// A soft ring that springs out of a toggle: the "springy" feedback for a click. One compositor animation, no
// timer; the next pulse replaces it. Its colour is the accent, so it follows the theme.
void ControlCenter::Impl::Pulse(Item const& it) {
    if (!fx || settings::Get().reduceMotion) return;
    fx.Children().RemoveAll();
    float rad = std::min(it.r.right - it.r.left, it.r.bottom - it.r.top) / 2 - ui::kFocusRing;
    auto geo = comp.CreateEllipseGeometry();
    geo.Radius({rad * s, rad * s});
    auto shape = comp.CreateSpriteShape(geo);
    shape.StrokeBrush(comp.CreateColorBrush(Col(theme::A(kAccent, 0.75f))));
    shape.StrokeThickness(ui::kFocusRing * s);
    auto v = comp.CreateShapeVisual();
    v.Size({1, 1});
    v.Offset({(it.r.left + it.r.right) / 2 * s, (it.r.top + it.r.bottom) / 2 * s, 0});
    v.Shapes().Append(shape);
    fx.Children().InsertAtTop(v);
    auto sc = comp.CreateVector3KeyFrameAnimation();
    sc.InsertKeyFrame(0.0f, {kPulseFrom, kPulseFrom, 1.0f});
    sc.InsertKeyFrame(1.0f, {kPulseTo, kPulseTo, 1.0f}, comp.CreateCubicBezierEasingFunction({0.2f, 0.8f}, {0.3f, 1.0f}));
    sc.Duration(Fast(ui::kPulseMs));
    v.StartAnimation(L"Scale", sc);
    auto op = comp.CreateScalarKeyFrameAnimation();
    op.InsertKeyFrame(0.0f, 1.0f);
    op.InsertKeyFrame(1.0f, 0.0f);
    op.Duration(Fast(ui::kPulseMs));
    v.StartAnimation(L"Opacity", op);
}

// One composition highlight that glides between controls (the surface itself is not redrawn on hover): the
// palette's fillHover, like every other panel.
void ControlCenter::Impl::SetHover(Item const* it) {
    if (!hv || !hvGeo) return;
    bool show = it && it->id != Id::Bright && it->id != Id::Vol;  // a slider is its own highlight
    if (!show) {
        if (hvShown) {
            hvShown = false;
            auto a = comp.CreateScalarKeyFrameAnimation();
            a.InsertKeyFrame(1.0f, 0.0f);
            a.Duration(Fast(ui::kHoverMs));
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
        o.Duration(Fast(ui::kHoverMs));
        hvGeo.StartAnimation(L"Offset", o);
        auto z = comp.CreateVector2KeyFrameAnimation();
        z.InsertKeyFrame(1.0f, sz, ease);
        z.Duration(Fast(ui::kHoverMs));
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
    a.Duration(Fast(ui::kHoverMs));
    hv.StartAnimation(L"Opacity", a);
}

// The pressed state of a control: the hover highlight jumps to the control and to fillPress in the same message that
// saw the button go down, with no animation to wait for. nullptr = released.
void ControlCenter::Impl::PressFeedback(Item const* it) {
    if (!hv || !hvBrush) return;
    const auto& p = theme::P();
    if (!it) { hvBrush.Color(Col(p.fillHover)); return; }
    hvBrush.Color(Col(p.fillPress));
    SetHover(it);
    hv.StopAnimation(L"Opacity");
    hv.Opacity(1);
}

// Whether a control can act now (an adapter, a hotspot, a microphone, a player to talk to). Disabled ones draw
// dimmed and neither highlight nor press; the keyboard skips them.
bool ControlCenter::Impl::Enabled(Item const& it) const {
    switch (it.id) {
        case Id::Wifi: case Id::WifiText: return radios.HasWifi();
        case Id::Bluetooth: case Id::BtText: return radios.HasBluetooth();
        case Id::Airplane: return radios.HasWifi() || radios.HasBluetooth();
        case Id::Hotspot: return hotspotAvail;
        case Id::Mic: return mic.Available();
        case Id::Battery: return mode != svc::PowerMode::Unknown;
        case Id::Prev: return Playing().has && Playing().canPrev;
        case Id::Next: return Playing().has && Playing().canNext;
        case Id::PlayPause: return Playing().has;
        default: return true;
    }
}

// Keyboard. The items are in reading order, so Tab is the next one that can act; Up / Down pick the nearest one in
// the row above or below. The ring shows only once the keyboard has been used.
void ControlCenter::Impl::FocusItem(Item const& it) {
    focus = it;  // a copy: Render() lays out again and invalidates `it`
    kbFocus = true;
    Render();
}

void ControlCenter::Impl::MoveFocus(int step) {
    std::vector<Item const*> order;
    for (auto const& it : items)
        if (!Passive(it.id) && Enabled(it)) order.push_back(&it);
    if (order.empty()) return;
    const size_t n = order.size();
    size_t at = n;
    for (size_t i = 0; i < n; ++i)
        if (Same(*order[i], focus)) at = i;
    FocusItem(*order[at == n ? (step > 0 ? 0 : n - 1) : (at + n + step) % n]);
}

void ControlCenter::Impl::MoveFocusDir(int dy) {
    Item const* from = nullptr;
    for (auto const& it : items)
        if (Same(it, focus) && focus.id != Id::None) from = &it;
    if (!from) { MoveFocus(dy); return; }
    const float fx = (from->r.left + from->r.right) / 2, fy = (from->r.top + from->r.bottom) / 2;
    Item const* best = nullptr;
    float bestScore = 0;
    for (auto const& it : items) {
        if (Passive(it.id) || !Enabled(it)) continue;
        const float along = ((it.r.top + it.r.bottom) / 2 - fy) * dy;
        if (along < ui::kS2) continue;  // not in the row above / below
        const float score = along + 2 * std::fabs((it.r.left + it.r.right) / 2 - fx);
        if (!best || score < bestScore) { best = &it; bestScore = score; }
    }
    if (best) FocusItem(*best);
}

void ControlCenter::Impl::NudgeSlider(int dir) {
    if (focus.id == Id::Vol) {
        if (!speaker.Available()) return;
        volume = std::clamp(volume + dir * kSliderStep, 0.0f, 1.0f);
        speaker.SetVolume(volume);
    } else if (focus.id == Id::Bright && bright >= 0) {
        bright = std::clamp(bright + dir * static_cast<int>(std::lround(kSliderStep * 100)), 0, 100);
        svc::brightness::Set(bright);
    } else {
        return;
    }
    Render();
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
            if (!radios.HasWifi()) return;  // no adapter (an Ethernet-only desktop): nothing to list
            outsOpen = powerOpen = false;
            listOffset = 0;
            expand = expand == Expand::Wifi ? Expand::None : Expand::Wifi;
            if (expand == Expand::Wifi) { wifiRows.clear(); svc::WifiRefresh(hwnd, kMsgWifi); }
            break;
        case Id::BtText:
            if (!radios.HasBluetooth()) return;
            outsOpen = powerOpen = false;
            listOffset = 0;
            expand = expand == Expand::Bluetooth ? Expand::None : Expand::Bluetooth;
            if (expand == Expand::Bluetooth) { btRows.clear(); btDone = false; svc::BtRefresh(hwnd, kMsgBt); }
            break;
        case Id::WifiNet:
            if (static_cast<size_t>(it.arg) >= wifiRows.size()) return;
            svc::WifiConnect(wifiRows[it.arg]);
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
        case Id::RadioSettings:
            LaunchAfterClose(expand == Expand::Wifi ? L"ms-settings:network-wifi" : L"ms-settings:bluetooth");
            return;
        case Id::Lock:
            if (svc::DryRun()) { svc::Log(L"DRYRUN lock workstation"); break; }
            Close();
            std::thread([] { Sleep(250); LockWorkStation(); }).detach();
            return;
        case Id::Power:
            expand = Expand::None;
            outsOpen = false;
            powerOpen = !powerOpen;
            break;
        case Id::PowSleep: RunPowerAfterClose(0); return;
        case Id::PowRestart: RunPowerAfterClose(1); return;
        case Id::PowShut: RunPowerAfterClose(2); return;
        case Id::Mic:
            if (!mic.Available()) return;
            micMuted = !micMuted;
            mic.SetMute(micMuted);
            Pulse(it);
            break;
        case Id::OutRow:
            expand = Expand::None;
            powerOpen = false;
            outsOpen = !outsOpen;
            outOffset = 0;
            break;
        case Id::Back:
            expand = Expand::None;
            outsOpen = false;
            listOffset = outOffset = 0;
            break;
        case Id::DiscoveryAction: {
            const bool wifi = expand == Expand::Wifi;
            const auto status = wifi ? svc::WifiStatus() : svc::BtStatus();
            if (status.phase == svc::DiscoveryPhase::Loading || status.phase == svc::DiscoveryPhase::Connecting) return;
            if (status.phase == svc::DiscoveryPhase::AccessDenied || status.phase == svc::DiscoveryPhase::Unavailable)
                LaunchAfterClose(wifi ? (status.phase == svc::DiscoveryPhase::AccessDenied ? L"ms-settings:privacy-location" : L"ms-settings:network-wifi") : L"ms-settings:bluetooth");
            else if (wifi) { wifiRows.clear(); svc::WifiRefresh(hwnd, kMsgWifi); }
            else { btRows.clear(); svc::BtRefresh(hot, kMsgBt); }
            break;
        }
        case Id::OutDev: {
            auto id = outs[it.arg].id;
            svc::SetDefaultOutput(id);
            for (auto& d : outs) d.isDefault = d.id == id;
            outsOpen = false;
            outOffset = 0;
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

void ControlCenter::Impl::ScrollDetails(int rows) {
    if (!outsOpen && expand == Expand::None) return;
    const size_t total = outsOpen ? outs.size() : expand == Expand::Wifi ? wifiRows.size() : btRows.size();
    const size_t visible = outsOpen ? outVisible : listVisible;
    size_t& offset = outsOpen ? outOffset : listOffset;
    const size_t limit = total - std::min(total, visible);
    const size_t next = static_cast<size_t>(std::clamp<int64_t>(static_cast<int64_t>(offset) + rows, 0, limit));
    if (next == offset) return;
    offset = next;
    pressed = hover = {};
    Render();
}

void ControlCenter::Impl::DragTo(float x) {
    for (auto const& it : items) {
        if (it.id != drag) continue;
        const float h = ui::kSliderH;
        float f = std::clamp((x - it.r.left - h / 2) / (it.r.right - it.r.left - h), 0.0f, 1.0f);
        if (drag == Id::Vol) {
            volume = f;
            speaker.SetVolume(f);
        } else {
            bright = static_cast<int>(std::lround(f * 100));
            ULONGLONG now = GetTickCount64();
            if (now - lastBrightSet >= 33) { lastBrightSet = now; svc::brightness::Set(bright); }
        }
        // The track follows the pointer 1:1: the slider module is redrawn on its own, straight away, with no
        // layout pass and no animation in between.
        auto tile = std::find_if(items.begin(), items.end(), [](Item const& i) { return i.id == Id::DisplayTile; });
        if (st == St::Open && !settling && tile != items.end()) RepaintRegion(tile->r);
        else Render();
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
    const bool dense = Dense() ||
        kHomeH + (outsOpen ? outs.size() * kListRowH + kS1 : 0) + (powerOpen ? kPowH + kS2 : 0) > MaxH();
    const float insetY = dense ? kS3 : kInset;
    // Grouped like the design: tight inside a group (the pills, the minis, the sliders), a larger step between groups.
    const float gap = kInnerGap;
    const float grp = dense ? kS2 : kGroupGap;
    const float headH = dense ? kButton - kS2 : kButton;
    const float pillH = dense ? kTileH - kS4 : kTileH;
    const float miniH = dense ? kRound : kRoundAction;
    const float rowH = dense ? kSliderHitH : kButton;  // a slider's row: the track (kSliderH) is centred in it
    const float mediaH = dense ? kMediaArt + kS2 : kMediaH;
    const float powerH = dense ? kRoundAction : kPowH;
    const float x0 = kInset, x1 = kW - kInset;
    float y = insetY;
    auto add = [&](Id id, D2D1_RECT_F r, int arg = 0, float rad = kRadiusHover) { items.push_back({id, r, arg, rad}); };
    // The footer: the battery and power mode at the left (a press cycles the mode), Lock, Settings and Power at the right.
    auto footer = [&](float top) {
        const float b = kButton;
        add(Id::Battery, {x0, top, x1 - 3 * b - 2 * kS1 - kS2, top + b});
        Id ids[] = {Id::Lock, Id::Settings, Id::Power};
        for (int i = 0; i < 3; ++i) {
            const float left = x1 - (3 - i) * b - (2 - i) * kS1;
            add(ids[i], {left, top, left + b, top + b});
        }
    };

    // Details get their own bounded page: expanding devices must never bury the shell actions.
    if (outsOpen || expand != Expand::None) {
        height = std::min(MaxH(), kHomeH);
        add(Id::Back, {x0, y, outsOpen ? x1 : x1 - kActW - kS2, y + kHeaderH});
        if (!outsOpen) add(Id::RadioSettings, {x1 - kActW, y + kS1, x1, y + kHeaderH - kS1});
        y += kHeaderH + kS2;
        const float footerTop = height - insetY - kButton;
        const float bottom = footerTop - kS3;
        add(Id::DeviceListTile, {x0, y, x1, bottom}, 0, kRadiusLarge);
        if (!outsOpen) add(Id::DiscoveryAction, {x1 - kActW - kS2, y + kS2, x1 - kS2, y + kS2 + kRound});
        const float rowTop = y + kS4 + kTitleLine + kS2 + (outsOpen ? 0 : kListRowH);
        const size_t capacity = static_cast<size_t>(std::max(0.0f, std::floor((bottom - kS2 - rowTop) / kListRowH)));
        const size_t total = outsOpen ? outs.size() : expand == Expand::Wifi ? wifiRows.size() : btRows.size();
        size_t& offset = outsOpen ? outOffset : listOffset;
        size_t& visible = outsOpen ? outVisible : listVisible;
        visible = std::min(total, capacity);
        offset = std::min(offset, total - visible);
        for (size_t row = 0; row < visible; ++row) {
            const float top = rowTop + row * kListRowH;
            add(outsOpen ? Id::OutDev : expand == Expand::Wifi ? Id::WifiNet : Id::BtDev,
                {x0 + kS2, top, x1 - kS2, top + kListRowH}, static_cast<int>(offset + row));
        }
        footer(footerTop);
        return;
    }

    // Header, on the glass itself: the time and date (opens the calendar and the notifications above the panel).
    add(Id::Clock, {x0, y, x1, y + headH});
    y += headH + grp;

    // Two connectivity pills: a round toggle, then the name over its state (the text side opens the list).
    const float col = (x1 - x0 - gap) / 2, rx = x0 + col + gap;
    auto pill = [&](float l, Id back, Id dot, Id text) {
        const float r = l + col, icx = l + kS3 + kRound / 2, icy = y + pillH / 2;
        add(back, {l, y, r, y + pillH}, 0, kRadiusLarge);
        add(dot, {icx - kRound / 2, icy - kRound / 2, icx + kRound / 2, icy + kRound / 2}, 0, kRound / 2);
        add(text, {icx + kRound / 2 + kS3, y + kS2, r - kS1, y + pillH - kS2});
    };
    pill(x0, Id::WifiPill, Id::Wifi, Id::WifiText);
    pill(rx, Id::BtPill, Id::Bluetooth, Id::BtText);
    y += pillH + grp;

    // Two rows of four small round toggles (Cast, Snip and the microphone sit among them).
    {
        const Id ids[2][4] = {{Id::Airplane, Id::Hotspot, Id::Dnd, Id::Night}, {Id::Dark, Id::Mic, Id::Snip, Id::Cast}};
        const float w = (x1 - x0 - 3 * gap) / 4;
        for (int row = 0; row < 2; ++row) {
            for (int c = 0; c < 4; ++c) {
                const float left = x0 + c * (w + gap);
                add(ids[row][c], {left, y, left + w, y + miniH}, 0, miniH / 2);
            }
            y += miniH + (row ? grp : gap);
        }
    }

    // The sliders: Display (when the panel has a brightness control), then Sound, with the audio output at its right
    // end. The icon sits outside the track, the track is kSliderH tall, and the Sound row is always laid out (dimmed
    // when there is no output), so the height never depends on the audio read.
    if (cfg().showSliders) {
        const bool disp = bright >= 0;
        const float trackL = x0 + kTitleLine + kS3, trackR = x1 - kButton - kS2;
        add(Id::DisplayTile, {x0, y, x1, y + (disp ? 2 : 1) * rowH}, 0, kRadiusLarge);  // the sliders' area (redrawn while one is dragged)
        float t = y;
        if (disp) {
            add(Id::Bright, {trackL, t, trackR, t + rowH});
            t += rowH;
        }
        add(Id::Vol, {trackL, t, trackR, t + rowH});
        add(Id::OutRow, {x1 - kButton, t + (rowH - kButton) / 2, x1, t + (rowH + kButton) / 2});
        y = t + rowH + grp;
    }

    // Media arrival never changes the home height. The compact power choices temporarily use its space.
    if (cfg().showMedia && !(dense && powerOpen)) {
        add(Id::MediaTile, {x0, y, x1, y + mediaH}, 0, kRadiusLarge);
        // One row of previous / play / next at the right, only while something plays (nothing to control otherwise).
        if (Playing().has) {
            const float cy = y + mediaH / 2, b = kButton - kS1, r = x1 - kS3;
            add(Id::Next, {r - b, cy - b / 2, r, cy + b / 2});
            add(Id::PlayPause, {r - 2 * b - kS1, cy - b / 2, r - b - kS1, cy + b / 2});
            add(Id::Prev, {r - 3 * b - 2 * kS1, cy - b / 2, r - 2 * b - 2 * kS1, cy + b / 2});
        }
        y += mediaH + grp;
    }

    footer(y);
    y += kButton + gap;
    // The inline Sleep / Restart / Shut down choice under the footer.
    if (powerOpen) {
        add(Id::PowerTile, {x0, y, x1, y + powerH}, 0, kRadiusLarge);
        Id ids[] = {Id::PowSleep, Id::PowRestart, Id::PowShut};
        float w3 = (x1 - x0 - 2 * kS2 - 2 * kS1) / 3;
        for (int i = 0; i < 3; i++) {
            float l = x0 + kS2 + i * (w3 + kS1);
            add(ids[i], {l, y + kS2, l + w3, y + powerH - kS2});
        }
        y += powerH + gap;
    }
    y += insetY - gap;
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
            SetHover(it && Enabled(*it) ? it : nullptr);
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
            a.Period(Fast(48));
            return a;
        };
        auto move = comp.CreateSpringVector3Animation();
        move.FinalValue(winrt::Windows::Foundation::IReference<wfn::float3>(at));
        move.DampingRatio(0.86f);
        move.Period(Fast(48));
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
            fade.Duration(Fast(2 * ui::kFastMs));
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
    if (ghost) { inner.Children().Remove(ghost); ghost = nullptr; }
    pill.Size({wPx, hPx});
    inner.Size({wPx, hPx});
    if (shadowV) shadowV.Size({wPx, hPx});
    clip.Size({wPx, hPx});
    rim.Size({wPx - s, hPx - s});
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
}

// Draws every item (or just `only`, minus `skip`) with the panel origin at (tx, ty) device px.
// With `region` (logical px, already on whole device px) only that part of the surface is cleared and redrawn.
void ControlCenter::Impl::Paint(ID2D1DeviceContext* dc, float tx, float ty, Item const* only, Item const* skip, D2D1_RECT_F const* region) {
    using namespace ui;
    dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(tx, ty));
    if (region) dc->PushAxisAlignedClip(*region, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->Clear(ColorF(0, 0.0f));
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    winrt::com_ptr<ID2D1SolidColorBrush> br;
    dc->CreateSolidColorBrush(kText, br.put());
    const auto& pal = theme::P();
    constexpr float kMarkStroke = kHair + kHair / 2;  // the dark-mode glyph's outline

    auto box = [&](D2D1_RECT_F r, float rad, D2D1_COLOR_F c) {
        br->SetColor(c);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br.get());
    };
    // A tile: one flat fill on the glass, the same in both themes. No border and no shading: the glass is the only
    // surface with an edge, so tiles read as parts of one panel instead of boxes in a box.
    auto tile = [&](D2D1_RECT_F r) { box(r, kRadiusLarge, pal.fill); };
    auto txt = [&](std::wstring const& t, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
        br->SetColor(c);
        dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), f, r, br.get());
    };
    auto glyph = [&](wchar_t g, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) { txt(std::wstring(1, g), f, r, c); };
    // A toggle: the accent when on, `idle` when off; a glyph, or the half-filled disc for dark mode. Unavailable = dimmed.
    auto toggle = [&](D2D1_RECT_F r, float rad, D2D1_COLOR_F idle, bool on, bool avail, wchar_t g, bool darkMark) {
        const float a = avail ? 1.0f : 0.4f;
        box(r, rad, theme::A(on ? kAccent : idle, a));
        const D2D1_COLOR_F ink = theme::A(on ? kOnAccent : kText, a);
        if (darkMark) {
            const float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, k = std::min(r.right - r.left, r.bottom - r.top) / 2 * 0.36f;
            br->SetColor(ink);
            dc->DrawEllipse(D2D1::Ellipse({cx, cy}, k, k), br.get(), kMarkStroke);
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
            glyph(g, fIcon.get(), r, ink);
        }
    };
    // Two lines: a title over a muted status, left-aligned in r and centred vertically.
    auto twoLines = [&](std::wstring const& title, std::wstring const& sub, D2D1_RECT_F r, float a) {
        const float cy = (r.top + r.bottom) / 2;
        txt(title, fTitle.get(), {r.left, cy - kTitleLine + 1, r.right, cy + 1}, theme::A(kText, a));
        txt(sub, fSub.get(), {r.left, cy + 1, r.right, cy + kCaptionLine + 1}, theme::A(kDim, a));
    };
    // A small toggle's glyph centred in a round control; a control without a plate at rest is just its glyph.
    auto round = [&](D2D1_RECT_F r, bool on, bool avail, wchar_t g, bool darkMark) {
        toggle(r, (r.right - r.left) / 2, kOff, on, avail, g, darkMark);
    };
    auto mini = [&](D2D1_RECT_F r, bool on, bool avail, wchar_t g, bool darkMark) {
        toggle(r, (r.bottom - r.top) / 2, pal.fill, on, avail, g, darkMark);
    };
    for (auto const& it : items) {
        if (only && !Same(it, *only)) continue;
        if (skip && Same(it, *skip)) continue;
        if (region && (it.r.right <= region->left || it.r.left >= region->right || it.r.bottom <= region->top || it.r.top >= region->bottom)) continue;
        auto const& r = it.r;
        const float cy = (r.top + r.bottom) / 2;
        const float on1 = Enabled(it) ? 1.0f : 0.4f;
        switch (it.id) {
            case Id::Back: {
                glyph(0xE72B, fIcon.get(), {r.left + kS2, r.top, r.left + kS2 + kS6, r.bottom}, kText);
                txt(L"Control Center", fTitle.get(), {r.left + kS2 + kS6 + kS2, r.top, r.right - (outsOpen ? kActW : kS2), r.bottom}, kText);
                const size_t total = outsOpen ? outs.size() : expand == Expand::Wifi ? wifiRows.size() : btRows.size();
                const size_t offset = outsOpen ? outOffset : listOffset, visible = outsOpen ? outVisible : listVisible;
                if (outsOpen && visible && total > visible) {
                    const auto range = std::to_wstring(offset + 1) + L"–" + std::to_wstring(offset + visible) + L" / " + std::to_wstring(total);
                    txt(range, fSubR.get(), {r.right - kActW, r.top, r.right - kS2, r.bottom}, kDim);
                }
                break;
            }
            case Id::RadioSettings:
                box(r, kRadiusHover, kOff);
                txt(L"Settings", fCap.get(), r, kText);
                break;
            case Id::DeviceListTile: {
                tile(r);
                const wchar_t* title = outsOpen ? L"Audio output" : expand == Expand::Wifi ? L"Wi-Fi networks" : L"Bluetooth devices";
                txt(title, fTitle.get(), {r.left + kS4, r.top + kS4, r.right - (outsOpen ? kS4 : kActW + kS4 + kS2), r.top + kS4 + kTitleLine}, kText);
                const size_t total = outsOpen ? outs.size() : expand == Expand::Wifi ? wifiRows.size() : btRows.size();
                const size_t offset = outsOpen ? outOffset : listOffset, visible = outsOpen ? outVisible : listVisible;
                if (visible && total > visible) {
                    const float top = r.top + kS4 + kTitleLine + kS2 + (outsOpen ? 0 : kListRowH);
                    const float trackH = r.bottom - kS2 - top;
                    const float thumbH = std::max(kThumbMin, trackH * visible / total);
                    const float thumbY = top + (trackH - thumbH) * offset / (total - visible);
                    box({r.right - kS2, thumbY, r.right - kS2 + kThumbW, thumbY + thumbH}, kThumbW / 2, kDim);
                }
                if (!outsOpen) {
                    const bool wifi = expand == Expand::Wifi;
                    const auto status = wifi ? svc::WifiStatus() : svc::BtStatus();
                    const wchar_t* message = wifi ? L"New networks open Windows Settings" : L"Select a device to open Bluetooth settings";
                    switch (status.phase) {
                        case svc::DiscoveryPhase::Idle: message = L"Refresh to find devices"; break;
                        case svc::DiscoveryPhase::Loading: message = wifi ? L"Looking for nearby networks…" : L"Looking for paired devices…"; break;
                        case svc::DiscoveryPhase::Connecting: message = L"Connecting to network…"; break;
                        case svc::DiscoveryPhase::Ready:
                            if (!total) message = wifi ? L"No networks found" : L"No paired devices";
                            if (wifi && status.error != ERROR_SUCCESS) message = L"Some adapters unavailable; other networks shown";
                            break;
                        case svc::DiscoveryPhase::AccessDenied: message = wifi ? L"Allow location access to scan for Wi-Fi" : L"Bluetooth access is blocked"; break;
                        case svc::DiscoveryPhase::Unavailable: message = wifi ? L"Wi-Fi is unavailable. Check the radio in Settings." : L"Bluetooth is unavailable. Check Settings."; break;
                        case svc::DiscoveryPhase::Failed: message = L"Couldn't refresh. Try again."; break;
                    }
                    txt(message, fSub.get(), {r.left + kS4, r.top + kS4 + kTitleLine + kS2, r.right - kS4, r.top + kS4 + kTitleLine + kS2 + kListRowH}, kDim);
                } else if (!total) {
                    txt(L"No audio outputs available", fSub.get(), {r.left + kS4, r.top + kS4 + kTitleLine + kS2, r.right - kS4, r.top + kS4 + kTitleLine + kS2 + kListRowH}, kDim);
                }
                break;
            }
            case Id::DiscoveryAction: {
                const bool wifi = expand == Expand::Wifi;
                const auto status = wifi ? svc::WifiStatus() : svc::BtStatus();
                const bool loading = status.phase == svc::DiscoveryPhase::Loading || status.phase == svc::DiscoveryPhase::Connecting;
                const wchar_t* label = status.phase == svc::DiscoveryPhase::Connecting ? L"Connecting…" : loading ? L"Scanning…" : status.phase == svc::DiscoveryPhase::AccessDenied || status.phase == svc::DiscoveryPhase::Unavailable ? L"Settings" : status.phase == svc::DiscoveryPhase::Failed ? L"Retry" : L"Refresh";
                box(r, kRadiusHover, kOff);
                txt(label, fCap.get(), r, theme::A(kText, loading ? 0.4f : 1.0f));
                break;
            }
            case Id::WifiPill: case Id::BtPill: case Id::MediaTile: case Id::PowerTile:
                tile(r);
                break;
            case Id::Clock: {
                wchar_t tb[32] = L"", db[64] = L"";
                SYSTEMTIME now;
                GetLocalTime(&now);
                lastMin = now.wMinute;
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS | TIME_NOTIMEMARKER, &now, nullptr, tb, 32);
                GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &now, L"dddd, d MMMM", db, 64, nullptr);
                txt(tb, fTime.get(), {r.left + kS2, r.top, r.right, r.bottom}, kText);
                txt(db, fSubR.get(), {r.left, r.top, r.right - kS2, r.bottom}, kDim);
                break;
            }
            case Id::Battery: {
                wchar_t g = 0xE83E;  // plug
                if (battery.hasBattery) g = static_cast<wchar_t>((battery.ac ? 0xEBAB : 0xEBA0) + std::min(10, battery.percent / 10));
                glyph(g, fIcon.get(), {r.left + kS2, r.top, r.left + kS2 + kTitleLine, r.bottom}, theme::A(kText, on1));
                std::wstring cap = battery.ac ? L"Plugged in" : L"Battery";
                switch (mode) {
                    case svc::PowerMode::Efficiency: cap = L"Efficiency"; break;
                    case svc::PowerMode::Balanced: cap = L"Balanced"; break;
                    case svc::PowerMode::Performance: case svc::PowerMode::Max: cap = L"Performance"; break;
                    default: break;
                }
                const float x = r.left + kS2 + kTitleLine + kS2;
                if (battery.hasBattery) twoLines(std::to_wstring(battery.percent) + L"%", cap, {x, r.top, r.right - kS2, r.bottom}, 1.0f);
                else txt(cap, fTitle.get(), {x, r.top, r.right - kS2, r.bottom}, kText);
                break;
            }
            case Id::WifiText: case Id::BtText: {
                bool wifi = it.id == Id::WifiText;
                bool avail = wifi ? radios.HasWifi() : radios.HasBluetooth(), on = wifi ? wifiOn : btOn;
                std::wstring sub;
                const wchar_t* missing = radios.Loading() ? L"Checking…" : radios.Failed() ? L"Check Settings" : L"Unavailable";
                if (wifi) sub = !avail ? missing : !on ? L"Off" : ssid.empty() ? L"On" : ssid;
                else {
                    sub = !avail ? missing : on ? L"On" : L"Off";
                    if (on) for (auto const& d : btRows) if (d.connected) { sub = d.name; break; }
                }
                twoLines(wifi ? L"Wi-Fi" : L"Bluetooth", sub, {r.left, r.top, r.right - kTitleLine, r.bottom}, avail ? 1.0f : 0.5f);
                bool open = expand == (wifi ? Expand::Wifi : Expand::Bluetooth);
                glyph(open ? 0xE70E : 0xE76C, fIconS.get(), {r.right - kTitleLine, cy - kTitleLine / 2, r.right, cy + kTitleLine / 2}, kDim);
                break;
            }
            case Id::Wifi: round(r, wifiOn, radios.HasWifi(), 0xE701, false); break;
            case Id::Bluetooth: round(r, btOn, radios.HasBluetooth(), 0xE702, false); break;
            case Id::Airplane: {
                bool avail = radios.HasWifi() || radios.HasBluetooth();
                mini(r, avail && !wifiOn && !btOn, avail, 0xE709, false);
                break;
            }
            case Id::Hotspot: mini(r, hotspotOn, hotspotAvail, 0xE704, false); break;
            case Id::Dnd: mini(r, dnd, true, 0xE708, false); break;
            case Id::Night: mini(r, night, true, 0xF08C, false); break;
            case Id::Dark: mini(r, dark, true, 0, true); break;
            case Id::Mic: mini(r, micMuted, mic.Available(), micMuted ? 0xEC54 : 0xE720, false); break;
            case Id::Snip: mini(r, false, true, 0xE8C6, false); break;
            case Id::Cast: mini(r, false, true, 0xE7F4, false); break;
            case Id::Settings: glyph(0xE713, fIcon.get(), r, kText); break;
            case Id::Lock: glyph(0xE72E, fIcon.get(), r, kText); break;
            case Id::Power:
                if (powerOpen) box(r, kRadiusHover, kAccent);
                glyph(0xE7E8, fIcon.get(), r, powerOpen ? kOnAccent : kText);
                break;
            case Id::PowSleep: case Id::PowRestart: case Id::PowShut:
                box(r, kRadiusSmall, kOff);
                txt(it.id == Id::PowSleep ? L"Sleep" : it.id == Id::PowRestart ? L"Restart" : L"Shut down", fBtn.get(), r, kText);
                break;
            case Id::Bright: case Id::Vol: {  // the icon outside the track at the left; the track kSliderH tall, centred in its row
                const float h = kSliderH, f = it.id == Id::Vol ? volume : bright / 100.0f;
                const bool avail = it.id == Id::Bright || speaker.Available();
                const D2D1_RECT_F tr{r.left, cy - h / 2, r.right, cy + h / 2};
                const wchar_t g = it.id == Id::Bright ? 0xE706 : (muted || volume <= 0.001f ? 0xE74F : 0xE767);
                glyph(g, fIcon.get(), {r.left - kS3 - kTitleLine, cy - kTitleLine / 2, r.left - kS3, cy + kTitleLine / 2}, theme::A(pal.textSoft, avail ? 1.0f : 0.5f));
                if (!avail) { box(tr, h / 2, theme::A(kOff, 0.5f)); break; }
                box(tr, h / 2, kOff);
                D2D1_RECT_F fr{tr.left, tr.top, tr.left + h + f * (tr.right - tr.left - h), tr.bottom};
                box(fr, h / 2, pal.sliderFill);
                if (!theme::Dark()) {  // a white fill needs its hairline on light glass
                    br->SetColor(theme::Wash(0.16f));
                    dc->DrawRoundedRectangle(D2D1::RoundedRect({fr.left + kHair / 2, fr.top + kHair / 2, fr.right - kHair / 2, fr.bottom - kHair / 2}, h / 2, h / 2), br.get(), kHair);
                }
                break;
            }
            case Id::OutRow:  // the audio output list: the chevron at the end of the Sound row
                glyph(0xE76C, fIconS.get(), r, theme::A(pal.textSoft, on1));
                break;
            case Id::OutDev: case Id::WifiNet: case Id::BtDev: {
                std::wstring name, sub;
                bool mark = false;
                const size_t have = it.id == Id::OutDev ? outs.size() : it.id == Id::WifiNet ? wifiRows.size() : btRows.size();
                if (static_cast<size_t>(it.arg) >= have) break;  // a list changed under a stale item: draw nothing, never index past the end
                if (it.id == Id::OutDev) { name = outs[it.arg].name; mark = outs[it.arg].isDefault; }
                else if (it.id == Id::WifiNet) {
                    auto const& n = wifiRows[it.arg];
                    name = n.ssid; mark = n.connected;
                    sub = n.connected ? L"Connected" : n.profileName.empty() ? L"Set up" : n.secure ? L"Secured" : L"Open";
                    for (int bar = 0; bar < 4; ++bar) {
                        const float left = r.left + kS2 + bar * kS1;
                        box({left, cy + kS2 - (bar + 1) * kS1, left + kS1 / 2, cy + kS2}, 0,
                            bar < n.bars ? kText : theme::A(kDim, 0.3f));
                    }
                } else {
                    auto const& d = btRows[it.arg];
                    name = d.name; mark = d.connected; sub = d.connected ? L"Connected" : L"Paired";
                }
                const float markL = r.right - kS2 - kS6;
                txt(name, fSub.get(), {r.left + kS2 + (it.id == Id::WifiNet ? kS6 : 0), r.top, sub.empty() ? markL : markL - kActW - kS1, r.bottom}, mark ? kText : kDim);
                if (!sub.empty()) txt(sub, fSubR.get(), {markL - kActW, r.top, markL - kS1, r.bottom}, kDim);
                if (mark) glyph(0xE73E, fIconS.get(), {markL, r.top, r.right - kS2, r.bottom}, kAccent);
                break;
            }
            case Id::Prev: case Id::PlayPause: case Id::Next: {
                auto const& ms = Playing();
                if (!ms.has) break;  // nothing to control: no controls
                wchar_t g = it.id == Id::Prev ? 0xE892 : it.id == Id::Next ? 0xE893 : (ms.playing ? 0xE769 : 0xE768);
                glyph(g, fIcon.get(), r, theme::A(kText, on1 < 1 ? 0.3f : 1.0f));
                break;
            }
            default: break;
        }
        if (it.id == Id::MediaTile) {
            // Artwork, then title over artist (each one line, ellipsis), then prev / play / next. Nothing playing:
            // a quiet placeholder and one explanatory line, with no controls.
            auto const& ms = Playing();
            const float art = std::min(kMediaArt, r.bottom - r.top - 2 * kS2);
            D2D1_RECT_F ar{r.left + kS3, cy - art / 2, r.left + kS3 + art, cy + art / 2};
            if (ms.has && ms.artSize > 0 && ms.art.size() == static_cast<size_t>(ms.artSize) * ms.artSize) {
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
                box(ar, kRadiusSmall, theme::A(kOff, ms.has ? 1.0f : 0.6f));
                glyph(0xE8D6, fIcon.get(), ar, theme::A(kDim, ms.has ? 1.0f : 0.7f));  // music note
            }
            const float tl = ar.right + kS3;
            if (ms.has) {
                float tr = r.right - kS3;
                for (auto const& b : items) if (b.id == Id::Prev) tr = b.r.left - kS2;
                const std::wstring title = ms.title.empty() ? L"Now Playing" : ms.title;
                if (ms.artist.empty()) txt(title, fTitle.get(), {tl, r.top, tr, r.bottom}, kText);
                else twoLines(title, ms.artist, {tl, r.top, tr, r.bottom}, 1.0f);
            } else {
                twoLines(L"Nothing playing", L"Play something and it shows up here", {tl, r.top, r.right - kS3, r.bottom}, 0.72f);
            }
        }
    }
    // Keyboard focus: an accent ring kFocusGap outside the control, only once the keyboard has been used.
    if (kbFocus && !only && focus.id != Id::None) {
        for (auto const& it : items) {
            if (!Same(it, focus)) continue;
            D2D1_RECT_F fr = it.r;
            float rad = it.rad;
            if (it.id == Id::Bright || it.id == Id::Vol) {
                const float cy = (fr.top + fr.bottom) / 2;
                fr = {fr.left, cy - kSliderH / 2, fr.right, cy + kSliderH / 2};
                rad = kSliderH / 2;
            }
            const float o = kFocusGap + kFocusRing / 2;
            br->SetColor(kAccent);
            dc->DrawRoundedRectangle(D2D1::RoundedRect({fr.left - o, fr.top - o, fr.right + o, fr.bottom + o}, rad + o, rad + o), br.get(), kFocusRing);
        }
    }
    if (region) dc->PopAxisAlignedClip();
}

// Redraws just `r` (logical px) of the panel surface: the slider drag's fast path. Items did not move, so no layout.
void ControlCenter::Impl::RepaintRegion(D2D1_RECT_F r) {
    if (!surf || st != St::Open) return;
    const RECT px{static_cast<LONG>(std::floor(r.left * s)), static_cast<LONG>(std::floor(r.top * s)),
                  static_cast<LONG>(std::ceil(r.right * s)), static_cast<LONG>(std::ceil(r.bottom * s))};
    auto interop = surf.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT off{};
    if (FAILED(interop->BeginDraw(&px, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return;
    try {
        // `off` is where the update rect's corner lands in the returned target, so the surface origin is at off - corner.
        const D2D1_RECT_F region{px.left / s, px.top / s, px.right / s, px.bottom / s};
        Paint(dc.get(), static_cast<float>(off.x - px.left), static_cast<float>(off.y - px.top), nullptr, nullptr, &region);
    } catch (...) {}
    interop->EndDraw();
}

// HALO_CC_SNAPSHOT=<file.png>: once each open has settled, the panel's drawing over a flat stand-in
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
    mk(fTime, ui::kFontDisplay, SB, ui::kHeading, L, M);
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
    hvBrush = comp.CreateColorBrush(Col(theme::P().fillHover));
    hs.FillBrush(hvBrush);
    hv.Shapes().Append(hs);
    inner.Children().InsertAtTop(hv);
    hvShown = false;

    fx = comp.CreateContainerVisual();
    fx.RelativeSizeAdjustment({1, 1});
    inner.Children().InsertAtTop(fx);

    const float t = ui::kHair * s;  // 1 px rim, inset by half its width so it stays inside the clip
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
    inner.Children().InsertAtTop(rimVisual);

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
