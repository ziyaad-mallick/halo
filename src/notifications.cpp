#include "notifications.h"

#include <d2d1_1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <set>
#include <thread>
#include <vector>

#include "effects.h"
#include "icons.h"
#include "motion.h"
#include "services.h"
#include "theme.h"

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
struct ICompositorDesktopInterop : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateDesktopWindowTarget(HWND hwnd, BOOL isTopmost, void** target) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnsureOnThread(DWORD threadId) = 0;
};
__CRT_UUID_DECL(ICompositorDesktopInterop, 0x29E691FA, 0x4567, 0x4DCA, 0xB3, 0x19, 0xD0, 0xF2, 0x07, 0xEB, 0x68, 0x07)

namespace wuc = winrt::Windows::UI::Composition;
namespace wun = winrt::Windows::UI::Notifications;
using namespace std::chrono_literals;
using D2D1::ColorF;
using winrt::Windows::Foundation::Numerics::float2;
using winrt::Windows::Foundation::Numerics::float3;

namespace halo {
namespace {

// Logical px; everything is scaled by the monitor DPI.
constexpr float kWStd = 360;          // panel width when standalone (attached, it follows the Control Center)
constexpr float kEdge = 10;           // gap to the screen edge (right and top)
constexpr float kPad = 14, kIcon = 32, kTextX = kPad + kIcon + 10, kRadius = 18, kGap = 10, kPeek = 10;
constexpr float kHeaderY = 12, kTitleY = 30, kBodyY = 48, kLine = 15, kMinH = 62, kCollapseH = 28;
constexpr float kHeadH = 38, kMoreH = 28, kEmptyH = 56;
constexpr float kCalHeadStd = 74, kCalNav = 26, kCalDow = 18, kCalRowStd = 26, kCalHStd = kCalHeadStd + kCalNav + 4 + kCalDow + 2 + 6 * kCalRowStd + 12;
constexpr int kMaxCards = 6, kMaxCardsAttached = 3;
constexpr float kAttachGap = 10, kAttachMargin = 8;  // below the Control Center; to the screen bottom
constexpr UINT kMsgData = WM_APP + 1;
constexpr UINT_PTR kTimerGone = 1, kTimerClose = 2;
constexpr wchar_t kClass[] = L"HaloNotifications", kMsgClass[] = L"HaloNotificationsMsg";

const D2D1_COLOR_F kText = theme::P().text, kDim = theme::P().dim;
// A subtle wash over glass: white-alpha in dark, a fainter ink wash in light.
D2D1_COLOR_F Wash(float darkAlpha) { return theme::Dark() ? ColorF(1, 1, 1, darkAlpha) : ColorF(0, 0, 0, darkAlpha * 0.5f); }

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
        auto mask = comp.CreateSurfaceBrush(surface);
        mask.Stretch(wuc::CompositionStretch::Fill);
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



struct IconPx {
    UINT w = 0, h = 0;
    std::vector<uint32_t> px;  // premultiplied BGRA
};

struct Notif {
    UINT id = 0;
    std::wstring appKey, aumid, app, title, body;
    LONGLONG ft = 0;  // FILETIME of creation
};
struct Snapshot {
    std::vector<Notif> items;
    std::map<std::wstring, IconPx> icons;  // appKey -> icon, first sight only
    bool ok = false;
    std::wstring error;
};

enum class K { Header, Notif, Group, Collapse, More, Empty, Calendar };

// What one card shows.
struct Row {
    std::wstring key;
    K kind = K::Notif;
    bool clearBtn = false;  // attached mode: the first card carries a small "Clear"
    int count = 1;  // Group: notifications in it. More: how many are not shown.
    std::wstring groupKey;
    std::vector<UINT> ids;  // what the x removes
    Notif n;
    float h = 0;
    float y = 0;
};

std::wstring Clean(std::wstring s) {
    if (s.rfind(L"ms-resource:", 0) == 0) return L"";
    return s;
}

std::wstring Rel(LONGLONG ft) {
    FILETIME n;
    GetSystemTimeAsFileTime(&n);
    LONGLONG now = (static_cast<LONGLONG>(n.dwHighDateTime) << 32) | n.dwLowDateTime;
    LONGLONG sec = std::max<LONGLONG>(0, (now - ft) / 10000000);
    if (sec < 60) return L"now";
    if (sec < 3600) return std::to_wstring(sec / 60) + L"m";
    if (sec < 86400) return std::to_wstring(sec / 3600) + L"h";
    return std::to_wstring(sec / 86400) + L"d";
}

// Decodes an encoded image to premultiplied BGRA.
bool Decode(std::vector<uint8_t> const& png, UINT& w, UINT& h, std::vector<uint32_t>& px) {
    winrt::com_ptr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put())))) return false;
    winrt::com_ptr<IWICStream> st;
    if (FAILED(f->CreateStream(st.put())) || FAILED(st->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size())))) return false;
    winrt::com_ptr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(st.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put()))) return false;
    winrt::com_ptr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, fr.put()))) return false;
    winrt::com_ptr<IWICFormatConverter> cv;
    if (FAILED(f->CreateFormatConverter(cv.put())) ||
        FAILED(cv->Initialize(fr.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return false;
    if (FAILED(cv->GetSize(&w, &h)) || !w || !h || w > 1024 || h > 1024) return false;
    px.resize(static_cast<size_t>(w) * h);
    return SUCCEEDED(cv->CopyPixels(nullptr, w * 4, static_cast<UINT>(px.size() * 4), reinterpret_cast<BYTE*>(px.data())));
}

// Box-filters an icon down to at most 64 px so the card draws it without aliasing.
void Shrink(IconPx& ic) {
    if (ic.w <= 64 && ic.h <= 64) return;
    const UINT nw = std::min<UINT>(ic.w, 64), nh = std::min<UINT>(ic.h, 64);
    std::vector<uint32_t> out(static_cast<size_t>(nw) * nh);
    for (UINT y = 0; y < nh; ++y)
        for (UINT x = 0; x < nw; ++x) {
            UINT x0 = x * ic.w / nw, x1 = std::max(x0 + 1, (x + 1) * ic.w / nw);
            UINT y0 = y * ic.h / nh, y1 = std::max(y0 + 1, (y + 1) * ic.h / nh);
            uint64_t a = 0, r = 0, g = 0, b = 0, n = 0;
            for (UINT yy = y0; yy < y1; ++yy)
                for (UINT xx = x0; xx < x1; ++xx) {
                    uint32_t p = ic.px[static_cast<size_t>(yy) * ic.w + xx];
                    a += p >> 24; r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF; ++n;
                }
            out[static_cast<size_t>(y) * nw + x] = static_cast<uint32_t>(a / n) << 24 | static_cast<uint32_t>(r / n) << 16 |
                                                  static_cast<uint32_t>(g / n) << 8 | static_cast<uint32_t>(b / n);
        }
    ic.w = nw; ic.h = nh; ic.px = std::move(out);
}

// (2) The shell's own AppsFolder item for the AUMID: what Start and the taskbar show.
bool ShellIcon(std::wstring const& aumid, IconPx& out) {
    winrt::com_ptr<IShellItem> item;
    if (FAILED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, aumid.c_str(), IID_PPV_ARGS(item.put())))) return false;
    auto factory = item.try_as<IShellItemImageFactory>();
    if (!factory) return false;
    HBITMAP hb = nullptr;
    if (FAILED(factory->GetImage(SIZE{64, 64}, SIIGBF_ICONONLY, &hb)) || !hb) return false;
    BITMAP bm{};
    bool ok = false;
    if (GetObjectW(hb, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        const int w = bm.bmWidth, h = bm.bmHeight;
        HDC dc = CreateCompatibleDC(nullptr);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
        if (dc && GetDIBits(dc, hb, 0, static_cast<UINT>(h), px.data(), &bmi, DIB_RGB_COLORS)) {
            bool hasAlpha = false, straight = false;
            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                if (px[i + 3]) hasAlpha = true;
                if (px[i] > px[i + 3] || px[i + 1] > px[i + 3] || px[i + 2] > px[i + 3]) straight = true;
            }
            for (size_t i = 0; i + 3 < px.size(); i += 4) {
                if (!hasAlpha) px[i + 3] = 255;  // an opaque bitmap
                else if (straight)
                    for (int c = 0; c < 3; ++c) px[i + c] = static_cast<uint8_t>((px[i + c] * px[i + 3] + 127) / 255);
            }
            out.w = w; out.h = h;
            out.px.resize(static_cast<size_t>(w) * h);
            memcpy(out.px.data(), px.data(), px.size());
            ok = true;
        }
        if (dc) DeleteDC(dc);
    }
    DeleteObject(hb);
    return ok;
}

// (3) icons.cpp: the unplated logo from the app's package, else the shell's jumbo icon.
bool PackagedIcon(std::wstring const& aumid, IconPx& out) {
    IconImage im;
    if (!LoadJumboIcon(L"shell:AppsFolder\\" + aumid, im, true) || im.width <= 0 || im.height <= 0) return false;
    out.w = im.width; out.h = im.height;
    out.px.resize(static_cast<size_t>(im.width) * im.height);
    memcpy(out.px.data(), im.bgra.data(), out.px.size() * 4);
    Shrink(out);
    return true;
}

// A few system toasts use a pseudo AUMID that has no AppsFolder entry; map them to the app that owns them.
std::wstring Alias(std::wstring const& aumid) {
    if (aumid == L"Windows.Defender.SecurityCenter") return L"Microsoft.SecHealthUI_8wekyb3d8bbwe!SecHealthUI";
    return aumid;
}

int DaysIn(int y, int m) {
    static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : d[m - 1];
}

}  // namespace

struct NotificationCenter::Impl {
    NotificationCenter* api;
    HINSTANCE inst;
    wuc::Compositor comp;
    wuc::CompositionGraphicsDevice gdev;
    bool dry = false;  // HALO_TEST / HALO_NOTIF_DRYRUN: log removals and launches instead of doing them

    // ---- data (UI thread) ----
    std::vector<Notif> all;
    std::map<std::wstring, IconPx> icons;
    std::set<std::wstring> expanded;
    std::set<UINT> hidden;  // dismissed locally and not (yet) gone from Windows
    bool haveData = false;

    // ---- worker ----
    HWND msgWnd{};
    std::thread worker;
    HANDLE evStop{}, evReq{};
    std::atomic<bool> visible{false};

    // ---- window + cards (exist only while open) ----
    struct Card {
        Row row;
        std::wstring sig;
        wuc::ContainerVisual outer{nullptr}, vis{nullptr};
        wuc::SpriteVisual content{nullptr};
        wuc::CompositionDrawingSurface surf{nullptr};
        wuc::CompositionRoundedRectangleGeometry clip{nullptr}, rim{nullptr};
        bool gone = false;
    };
    enum class St { Closed, Open, Closing } st = St::Closed;
    HWND hwnd{};
    float s = 1;
    RECT work{};
    float availH = 800;  // logical px a column of content may use
    // Geometry. Standalone these are the constants; attached to the Control Center the width is the CC's
    // and the calendar drops its big date (the CC header has it) and may use shorter rows.
    float kW = kWStd, kCalHead = kCalHeadStd, kCalRow = kCalRowStd, kCalH = kCalHStd;
    bool attached = false;
    RECT anchorRc{};  // the Control Center panel on screen (physical px)
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr}, panelVis{nullptr};
    wuc::CompositionBrush noise{nullptr};
    std::vector<Card> cards;
    std::vector<Card> dying;
    float totalH = 0, winH = 0;
    int hoverCard = -1, hoverPart = 0;
    bool tracking = false;

    // calendar
    SYSTEMTIME today{};
    int calYear = 0, calMonth = 0;
    D2D1_COLOR_F accent = theme::P().accent;

    winrt::com_ptr<IDWriteFactory> dw;
    winrt::com_ptr<IDWriteTextFormat> fApp, fTime, fTitle, fBody, fPill, fCount, fHead, fClear, fBig, fCalDow, fNav, fDay, fDayB, fDow;

    Impl(NotificationCenter* a, HINSTANCE i, wuc::Compositor c, wuc::CompositionGraphicsDevice g) : api(a), inst(i), comp(std::move(c)), gdev(std::move(g)) {}

    void Start();
    void Stop();
    void Worker();
    Snapshot Query(std::set<std::wstring>& iconDone);
    void OnSnapshot(Snapshot& snap);

    void Open();
    void Close();
    void Teardown();
    void Focus();
    void BuildFonts();
    std::vector<Row> BuildRows();
    void Update(bool firstShow);
    void CreateCard(Card& c);
    void Draw(Card& c, bool hover, int part);
    void DrawNotif(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, Row const& r, bool hover, bool closeHover, bool clearHover);
    void DrawHeader(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, Row const& r, int part);
    void DrawCalendar(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, int part);
    void Place();
    void ApplyGeometry();
    void Dismissed() { if (api->onDismiss) api->onDismiss(); else Close(); }
    float2 Anchor() const { return attached ? float2{std::ceil(kW * s), 0} : float2{(kW + motion::kPad) * s, (totalH / 2 + motion::kPad) * s}; }
    float2 Travel() const { return attached ? float2{0, -28.0f * s} : float2{48.0f * s, 0}; }  // attached: the Control Center's motion
    void AnimateIn(Card& c, int order);
    void AnimateOut(Card& c);
    int CardAt(float x, float y, int* part);
    int Visible() const;
    void Dismiss(Row const& r);
    void Activate(Row const& r);
    void ClearAll();
    void ShiftMonth(int d);
    float MeasureBody(std::wstring& body, float width, int* lines);
    wuc::CompositionBrush NoiseBrush(int w, int h);

    LRESULT Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK Thunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK MsgThunk(HWND, UINT, WPARAM, LPARAM);
};

// ---------------------------------------------------------------------------------------------
// worker: listener queries, woken by a change notification on the notification database folder

Snapshot NotificationCenter::Impl::Query(std::set<std::wstring>& iconDone) {
    Snapshot snap;
    try {
        auto l = wun::Management::UserNotificationListener::Current();
        if (l.GetAccessStatus() != wun::Management::UserNotificationListenerAccessStatus::Allowed) {
            auto a = l.RequestAccessAsync().get();
            if (a != wun::Management::UserNotificationListenerAccessStatus::Allowed) {
                snap.error = L"access status " + std::to_wstring(static_cast<int>(a));
                return snap;
            }
        }
        auto list = l.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
        for (auto const& un : list) {
            Notif n;
            n.id = un.Id();
            auto info = un.AppInfo();
            n.aumid = info.AppUserModelId().c_str();
            n.app = info.DisplayInfo().DisplayName().c_str();
            n.appKey = n.aumid.empty() ? n.app : n.aumid;
            {
                FILETIME f = winrt::clock::to_file_time(un.CreationTime());
                n.ft = (static_cast<LONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
            }
            std::vector<std::wstring> texts;
            try {
                auto vis = un.Notification().Visual();
                auto b = vis.GetBinding(L"ToastGeneric");
                if (!b && vis.Bindings().Size()) b = vis.Bindings().GetAt(0);
                if (b)
                    for (auto const& t : b.GetTextElements()) {
                        std::wstring x = Clean(t.Text().c_str());
                        if (!x.empty()) texts.push_back(x);
                    }
            } catch (...) {}
            if (!texts.empty()) n.title = texts[0];
            for (size_t i = 1; i < texts.size(); ++i) n.body += (i > 1 ? L"\n" : L"") + texts[i];
            snap.items.push_back(std::move(n));
            auto const& cur = snap.items.back();
            if (iconDone.count(cur.appKey)) continue;
            iconDone.insert(cur.appKey);

            // Icon sources, in order: (1) AppInfo logo, (2) the shell's AppsFolder item, (3) the package's
            // own logo; only then does the card draw a letter tile. Cached per app key.
            IconPx ic;
            const wchar_t* src = nullptr;
            try {
                auto ref = info.DisplayInfo().GetLogo({72, 72});  // null for some apps (Windows Security)
                if (ref) {
                    auto stream = ref.OpenReadAsync().get();
                    winrt::Windows::Storage::Streams::DataReader rd(stream);
                    auto size = static_cast<uint32_t>(stream.Size());
                    if (size && size < (4u << 20)) {
                        rd.LoadAsync(size).get();
                        std::vector<uint8_t> bytes(size);
                        rd.ReadBytes(bytes);
                        if (Decode(bytes, ic.w, ic.h, ic.px)) src = L"AppInfo.GetLogo";
                    }
                }
            } catch (...) {}
            if (!src && !cur.aumid.empty()) {
                ic = {};
                if (ShellIcon(Alias(cur.aumid), ic)) src = L"shell AppsFolder GetImage";
            }
            if (!src && !cur.aumid.empty()) {
                ic = {};
                if (PackagedIcon(Alias(cur.aumid), ic)) src = L"packaged logo (icons.cpp)";
            }
            svc::Log(L"notifications: icon key=" + cur.appKey + L" app=" + cur.app + L" source=" + (src ? src : L"NONE (letter tile)"));
            if (src) snap.icons[cur.appKey] = std::move(ic);
        }
        snap.ok = true;
    } catch (winrt::hresult_error const& e) {
        snap.error = L"0x" + std::to_wstring(static_cast<unsigned>(e.code().value)) + L" " + e.message().c_str();
    }
    return snap;
}

// The thread sleeps on the folder watch; a change while the panel is closed is re-armed and ignored.
void NotificationCenter::Impl::Worker() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    wchar_t local[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring dir = std::wstring(local) + L"\\Microsoft\\Windows\\Notifications";
    HANDLE watch = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (watch == INVALID_HANDLE_VALUE) { svc::Log(L"notifications: cannot watch " + dir); watch = nullptr; }
    std::set<std::wstring> iconDone;
    HANDLE h[3] = {evStop, evReq, watch};
    for (;;) {
        DWORD r = WaitForMultipleObjects(watch ? 3 : 2, h, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 2) {
            FindNextChangeNotification(watch);
            if (!visible) continue;  // the next Open asks for a fresh read anyway
            // The database writes in bursts; settle first.
            for (int i = 0; i < 10 && WaitForSingleObject(watch, 250) == WAIT_OBJECT_0; ++i) FindNextChangeNotification(watch);
            svc::Log(L"notifications: database changed");
        }
        if (!visible) continue;
        auto* snap = new Snapshot(Query(iconDone));
        if (!msgWnd || !PostMessageW(msgWnd, kMsgData, 0, reinterpret_cast<LPARAM>(snap))) delete snap;
    }
    if (watch) FindCloseChangeNotification(watch);
}

void NotificationCenter::Impl::Start() {
    dry = _wgetenv(L"HALO_TEST") || _wgetenv(L"HALO_NOTIF_DRYRUN");
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = inst;
    wc.lpfnWndProc = MsgThunk;
    wc.lpszClassName = kMsgClass;
    RegisterClassExW(&wc);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = Thunk;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    msgWnd = CreateWindowExW(0, kMsgClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, this);
    evStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    evReq = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    worker = std::thread([this] { Worker(); });
}

void NotificationCenter::Impl::Stop() {
    Teardown();
    SetEvent(evStop);
    if (worker.joinable()) worker.join();
    HWND mw = msgWnd;
    msgWnd = nullptr;
    if (mw) DestroyWindow(mw);
    CloseHandle(evStop);
    CloseHandle(evReq);
}

LRESULT CALLBACK NotificationCenter::Impl::MsgThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == kMsgData) {
        std::unique_ptr<Snapshot> snap(reinterpret_cast<Snapshot*>(l));
        if (self) self->OnSnapshot(*snap);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void NotificationCenter::Impl::OnSnapshot(Snapshot& snap) {
    if (!snap.ok) { svc::Log(L"notifications: read failed: " + snap.error); return; }
    for (auto& [k, ic] : snap.icons) icons[k] = std::move(ic);
    std::sort(snap.items.begin(), snap.items.end(), [](Notif const& a, Notif const& b) { return a.ft > b.ft; });
    all = std::move(snap.items);
    std::set<UINT> present;
    for (auto& n : all) present.insert(n.id);
    for (auto it = hidden.begin(); it != hidden.end();) it = present.count(*it) ? std::next(it) : hidden.erase(it);
    haveData = true;
    svc::Log(L"notifications: " + std::to_wstring(all.size()) + L" in Action Center");
    if (st == St::Open) Update(false);
}

// ---------------------------------------------------------------------------------------------
// view model

int NotificationCenter::Impl::Visible() const {
    int n = 0;
    for (auto const& x : all) n += hidden.count(x.id) ? 0 : 1;
    return n;
}

// Notification rows only (header, "more" and calendar are added by Update).
std::vector<Row> NotificationCenter::Impl::BuildRows() {
    // Groups ordered by their newest notification.
    std::vector<std::wstring> order;
    std::map<std::wstring, std::vector<Notif const*>> groups;
    for (auto const& n : all) {
        if (hidden.count(n.id)) continue;
        if (!groups.count(n.appKey)) order.push_back(n.appKey);
        groups[n.appKey].push_back(&n);
    }
    std::vector<Row> rows;
    for (auto const& k : order) {
        auto& g = groups[k];
        bool open = g.size() == 1 || expanded.count(k);
        if (open) {
            for (auto* n : g) {
                Row r;
                r.key = L"n" + std::to_wstring(n->id);
                r.groupKey = k;
                r.ids = {n->id};
                r.n = *n;
                rows.push_back(std::move(r));
            }
            if (g.size() > 1) {
                Row r;
                r.key = L"c" + k;
                r.kind = K::Collapse;
                r.groupKey = k;
                rows.push_back(std::move(r));
            }
        } else {
            Row r;
            r.key = L"g" + k;
            r.kind = K::Group;
            r.groupKey = k;
            r.count = static_cast<int>(g.size());
            for (auto* n : g) r.ids.push_back(n->id);
            r.n = *g[0];
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

float NotificationCenter::Impl::MeasureBody(std::wstring& body, float width, int* lines) {
    *lines = 0;
    if (body.empty() || !fBody) return 0;
    for (int guard = 0; guard < 40; ++guard) {
        winrt::com_ptr<IDWriteTextLayout> lay;
        if (FAILED(dw->CreateTextLayout(body.c_str(), static_cast<UINT32>(body.size()), fBody.get(), width, 1000, lay.put()))) return 0;
        DWRITE_TEXT_METRICS tm{};
        lay->GetMetrics(&tm);
        if (tm.lineCount <= 2) { *lines = static_cast<int>(tm.lineCount); return kLine * tm.lineCount; }
        // Too long: cut to roughly two lines, ending in an ellipsis, and measure again.
        UINT32 n = 0;
        lay->GetLineMetrics(nullptr, 0, &n);
        std::vector<DWRITE_LINE_METRICS> lm(n);
        lay->GetLineMetrics(lm.data(), n, &n);
        size_t keep = lm[0].length + lm[1].length;
        if (guard) keep = body.size() - 2;
        while (keep > 1 && (body[keep - 1] == L' ' || body[keep - 1] == L'\n' || body[keep - 1] == L'\r' || body[keep - 1] == L'.' || body[keep - 1] == L'…')) --keep;
        body = body.substr(0, keep > 1 ? keep - 1 : keep) + L"…";
    }
    *lines = 2;
    return kLine * 2;
}

void NotificationCenter::Impl::ShiftMonth(int d) {
    int m = calMonth + d, y = calYear;
    if (m < 1) { m = 12; --y; }
    if (m > 12) { m = 1; ++y; }
    calYear = y; calMonth = m;
    for (auto& c : cards)
        if (c.row.kind == K::Calendar) Draw(c, false, 0);
}

// ---------------------------------------------------------------------------------------------
// window

LRESULT CALLBACK NotificationCenter::Impl::Thunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Proc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

// x, y in panel coordinates (logical). part: 0 body, 1 x, 2 Clear, 3 prev month, 4 next month, 5 month label, -1 inert.
int NotificationCenter::Impl::CardAt(float x, float y, int* part) {
    if (part) *part = -1;
    for (size_t i = 0; i < cards.size(); ++i) {
        auto const& r = cards[i].row;
        if (cards[i].gone || x < 0 || x >= kW || y < r.y || y >= r.y + r.h) continue;
        if (!part) return static_cast<int>(i);
        const float ly = y - r.y;
        switch (r.kind) {
            case K::Notif: case K::Group: {
                float cx = kW - kPad - 9, cy = kHeaderY + 8;
                *part = (x - cx) * (x - cx) + (ly - cy) * (ly - cy) <= 11 * 11 ? 1 : 0;
                if (r.clearBtn && x >= kW - kPad - 118 && x < kW - kPad - 70 && ly >= kHeaderY - 3 && ly < kHeaderY + 17) *part = 2;
                break;
            }
            case K::Collapse: *part = 0; break;
            case K::Header: *part = Visible() && x >= kW - kPad - 58 - 4 ? 2 : -1; break;
            case K::Calendar: {
                const float ny = kCalHead;
                if (ly >= ny && ly < ny + kCalNav) {
                    float nx1 = kW - kPad - 26, nx0 = nx1 - 28;
                    *part = x >= nx1 ? 4 : x >= nx0 ? 3 : x < kW / 2 ? 5 : -1;
                }
                break;
            }
            default: break;
        }
        return static_cast<int>(i);
    }
    return -1;
}

LRESULT NotificationCenter::Impl::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const float x = static_cast<short>(LOWORD(l)) / s - motion::kPad, y = static_cast<short>(HIWORD(l)) / s - motion::kPad;
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && st == St::Open) {  // clicked outside, unless it was the Control Center
                HWND other = reinterpret_cast<HWND>(l);
                if (other && api->siblingHwnd && other == api->siblingHwnd()) return 0;
                Dismissed();
            }
            return 0;
        case WM_KEYDOWN:
            if (w == VK_ESCAPE) Dismissed();
            return 0;
        case WM_NCHITTEST: {
            POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
            ScreenToClient(h, &p);
            return CardAt(p.x / s - motion::kPad, p.y / s - motion::kPad, nullptr) >= 0 ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_MOUSEMOVE: {
            if (!tracking && !_wgetenv(L"HALO_TEST")) {  // test hook: posted moves must not be undone by a real WM_MOUSELEAVE
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                tracking = TrackMouseEvent(&tme);
            }
            int part = -1;
            int i = CardAt(x, y, &part);
            if (i != hoverCard || part != hoverPart) {
                int old = hoverCard;
                hoverCard = i;
                hoverPart = part;
                if (old >= 0 && old < static_cast<int>(cards.size())) Draw(cards[old], false, -1);
                if (i >= 0) Draw(cards[i], true, part);
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            tracking = false;
            int old = hoverCard;
            hoverCard = -1;
            hoverPart = -1;
            if (old >= 0 && old < static_cast<int>(cards.size())) Draw(cards[old], false, -1);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (st != St::Open) return 0;
            int part = -1;
            int i = CardAt(x, y, &part);
            if (i < 0) return 0;
            Row r = cards[i].row;  // copy: Update() rebuilds the cards
            switch (r.kind) {
                case K::Collapse: expanded.erase(r.groupKey); Update(false); break;
                case K::Notif: if (part == 1) Dismiss(r); else if (part == 2) ClearAll(); else Activate(r); break;
                case K::Group: if (part == 1) Dismiss(r); else if (part == 2) ClearAll(); else { expanded.insert(r.groupKey); Update(false); } break;
                case K::Header: if (part == 2) ClearAll(); break;
                case K::Calendar:
                    if (part == 3) ShiftMonth(-1);
                    else if (part == 4) ShiftMonth(1);
                    else if (part == 5) { calYear = today.wYear; calMonth = today.wMonth; ShiftMonth(0); }
                    break;
                default: break;
            }
            return 0;
        }
        case WM_TIMER:
            if (w == kTimerGone) {
                KillTimer(h, kTimerGone);
                dying.clear();
                Place();
            } else if (w == kTimerClose) {
                Teardown();
            }
            return 0;
        case WM_NCDESTROY: hwnd = nullptr; break;
    }
    return DefWindowProcW(h, m, w, l);
}

void NotificationCenter::Impl::Dismiss(Row const& r) {
    for (UINT id : r.ids) {
        hidden.insert(id);
        if (dry) { svc::Log(L"notifications: DRY RUN, would remove id " + std::to_wstring(id)); continue; }
        try { wun::Management::UserNotificationListener::Current().RemoveNotification(id); }
        catch (winrt::hresult_error const& e) { svc::Log(L"notifications: RemoveNotification failed, hidden locally: " + std::wstring(e.message().c_str())); }
    }
    hoverCard = -1;
    Update(false);
}

void NotificationCenter::Impl::ClearAll() {
    Row r;
    for (auto const& n : all)
        if (!hidden.count(n.id)) r.ids.push_back(n.id);
    svc::Log(L"notifications: clear all (" + std::to_wstring(r.ids.size()) + L")");
    Dismiss(r);
}

void NotificationCenter::Impl::Activate(Row const& r) {
    if (dry) { svc::Log(L"notifications: DRY RUN, would open " + r.n.aumid + L" and remove id " + std::to_wstring(r.n.id)); return; }
    if (!r.n.aumid.empty()) {
        auto res = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + r.n.aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        svc::Log(L"notifications: open " + r.n.aumid + L" -> " + std::to_wstring(res));
    }
    Dismiss(r);  // Windows removes a toast once it has been clicked
}

void NotificationCenter::Impl::BuildFonts() {
    if (dw) return;
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.put())));
    auto mk = [&](winrt::com_ptr<IDWriteTextFormat>& out, DWRITE_FONT_WEIGHT wt, float size, DWRITE_TEXT_ALIGNMENT al, bool wrap, bool mid = false) {
        winrt::check_hresult(dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out.put()));
        out->SetTextAlignment(al);
        out->SetParagraphAlignment(mid ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            winrt::com_ptr<IDWriteInlineObject> sign;
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(out.get(), sign.put()))) out->SetTrimming(&trim, sign.get());
        }
    };
    const auto SB = DWRITE_FONT_WEIGHT_SEMI_BOLD, R = DWRITE_FONT_WEIGHT_NORMAL;
    const auto L_ = DWRITE_TEXT_ALIGNMENT_LEADING, C = DWRITE_TEXT_ALIGNMENT_CENTER;
    mk(fApp, SB, 11, L_, false);
    mk(fTime, R, 11, DWRITE_TEXT_ALIGNMENT_TRAILING, false);
    mk(fTitle, SB, 12.5f, L_, false);
    mk(fBody, R, 11.5f, L_, true);
    mk(fPill, SB, 11.5f, C, false, true);
    mk(fCount, SB, 10.5f, C, false, true);
    mk(fHead, SB, 15, L_, false, true);
    mk(fClear, SB, 12, C, false, true);
    mk(fBig, SB, 28, L_, false);
    mk(fCalDow, SB, 11.5f, L_, false);
    mk(fNav, SB, 13, L_, false, true);
    mk(fDay, R, 12.5f, C, false, true);
    mk(fDayB, SB, 12.5f, C, false, true);
    mk(fDow, SB, 10.5f, C, false, true);
}

wuc::CompositionBrush NotificationCenter::Impl::NoiseBrush(int w, int h) {
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

// ---------------------------------------------------------------------------------------------
// cards

// Same glass recipe as the dock pill and the Control Center: blurred backdrop, #16161A tint at 50%,
// 2.5% grain, 1 px gradient rim.
void NotificationCenter::Impl::CreateCard(Card& c) {
    const float wPx = std::ceil(kW * s), hPx = std::ceil(c.row.h * s);
    const bool group = c.row.kind == K::Group;
    c.outer = comp.CreateContainerVisual();
    c.outer.Size({wPx, hPx + (group ? kPeek * s : 0)});
    c.outer.CenterPoint({wPx / 2, 0, 0});

    if (group) {  // the rest of the stack peeking out from under the card
        for (int i = 2; i >= 1; --i) {
            auto peek = comp.CreateSpriteVisual();
            float inset = (7.0f * i) * s;
            peek.Size({wPx - 2 * inset, 20 * s});
            peek.Offset({inset, hPx + (i == 1 ? 5.0f : 10.0f) * s - 20 * s, 0});
            peek.Brush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(theme::Dark() ? (i == 1 ? 0x30 : 0x1A) : (i == 1 ? 0x78 : 0x48), 0xFF, 0xFF, 0xFF)));
            auto g = comp.CreateRoundedRectangleGeometry();
            g.CornerRadius({kRadius * s, kRadius * s});
            g.Size(peek.Size());
            peek.Clip(comp.CreateGeometricClip(g));
            c.outer.Children().InsertAtTop(peek);
        }
    }

    {
        const float rr = (c.row.kind == K::Collapse || c.row.kind == K::More || c.row.kind == K::Header) ? c.row.h / 2 * s : kRadius * s;
        auto col = theme::P().shadow;
        col.a = static_cast<uint8_t>(col.a * 0.7f);
        if (auto sh = MakeShadow(comp, gdev, wPx, hPx, rr, theme::P().shadowBlur * 0.5f * s, 3.0f * s, col)) c.outer.Children().InsertAtTop(sh);
    }
    c.vis = comp.CreateContainerVisual();
    c.vis.Size({wPx, hPx});
    c.outer.Children().InsertAtTop(c.vis);

    auto glass = comp.CreateSpriteVisual();
    glass.RelativeSizeAdjustment({1, 1});
    try {
        auto blur = winrt::make<GaussianBlurEffect>(wuc::CompositionEffectSourceParameter(L"backdrop"), 40.0f);
        auto brush = comp.CreateEffectFactory(blur.as<winrt::Windows::Graphics::Effects::IGraphicsEffect>()).CreateBrush();
        brush.SetSourceParameter(L"backdrop", comp.CreateHostBackdropBrush());
        glass.Brush(brush);
    } catch (...) { svc::Log(L"notifications: blur brush failed"); }
    c.vis.Children().InsertAtTop(glass);

    auto tint = comp.CreateSpriteVisual();
    tint.RelativeSizeAdjustment({1, 1});
    tint.Brush(comp.CreateColorBrush(theme::Dark() ? theme::C(theme::P().tint) : winrt::Windows::UI::ColorHelper::FromArgb(0xF0, 0xFA, 0xFA, 0xFC)));
    c.vis.Children().InsertAtTop(tint);

    if (noise) {
        auto n = comp.CreateSpriteVisual();
        n.RelativeSizeAdjustment({1, 1});
        n.Opacity(theme::P().noise);
        n.Brush(noise);
        c.vis.Children().InsertAtTop(n);
    }

    c.surf = gdev.CreateDrawingSurface({wPx, hPx}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                       winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
    c.content = comp.CreateSpriteVisual();
    c.content.Size({wPx, hPx});
    auto sb = comp.CreateSurfaceBrush(c.surf);
    sb.Stretch(wuc::CompositionStretch::None);
    c.content.Brush(sb);
    c.vis.Children().InsertAtTop(c.content);

    const float t = s;
    const bool pill = c.row.kind == K::Collapse || c.row.kind == K::More || c.row.kind == K::Header;
    const float rad = pill ? c.row.h / 2 * s : kRadius * s;
    c.rim = comp.CreateRoundedRectangleGeometry();
    c.rim.Offset({t / 2, t / 2});
    c.rim.CornerRadius({rad - t / 2, rad - t / 2});
    c.rim.Size({wPx - t, hPx - t});
    auto gradient = comp.CreateLinearGradientBrush();
    gradient.MappingMode(wuc::CompositionMappingMode::Relative);
    gradient.StartPoint({0, 0});
    gradient.EndPoint({0, 1});
    auto stop = [&](float at, uint8_t a) {
        gradient.ColorStops().Append(comp.CreateColorGradientStop(at, winrt::Windows::UI::ColorHelper::FromArgb(a, 0xFF, 0xFF, 0xFF)));
    };
    stop(0.0f, 0x70); stop(0.35f, 0x18); stop(0.7f, 0x0A); stop(1.0f, 0x26);
    if (!theme::Dark()) { gradient.ColorStops().Clear(); for (auto [at, c] : {std::pair{0.0f, theme::P().rimTop}, {0.35f, theme::P().rimMid}, {0.7f, theme::P().rimLow}, {1.0f, theme::P().rimBottom}}) gradient.ColorStops().Append(comp.CreateColorGradientStop(at, theme::C(c))); }
    auto sprite = comp.CreateSpriteShape(c.rim);
    sprite.StrokeBrush(gradient);
    sprite.StrokeThickness(t);
    auto rimVisual = comp.CreateShapeVisual();
    rimVisual.RelativeSizeAdjustment({1, 1});
    rimVisual.Shapes().Append(sprite);
    if (theme::P().edge.a) {  // hairline edge for light glass
        auto edge = comp.CreateSpriteShape(c.rim);
        edge.StrokeBrush(comp.CreateColorBrush(theme::C(theme::P().edge)));
        edge.StrokeThickness(t);
        rimVisual.Shapes().Append(edge);
    }
    c.vis.Children().InsertAtTop(rimVisual);

    c.clip = comp.CreateRoundedRectangleGeometry();
    c.clip.CornerRadius({rad, rad});
    c.clip.Size({wPx, hPx});
    c.vis.Clip(comp.CreateGeometricClip(c.clip));
    Draw(c, false, -1);
}

void NotificationCenter::Impl::Draw(Card& c, bool hover, int part) {
    if (!c.surf) return;
    auto interop = c.surf.as<ICompositionDrawingSurfaceInterop>();
    winrt::com_ptr<ID2D1DeviceContext> dc;
    POINT off{};
    if (FAILED(interop->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), dc.put_void(), &off))) return;
    try {
        auto const& r = c.row;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(s, s) * D2D1::Matrix3x2F::Translation(static_cast<float>(off.x), static_cast<float>(off.y)));
        dc->Clear(ColorF(0, 0.0f));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        winrt::com_ptr<ID2D1SolidColorBrush> br;
        dc->CreateSolidColorBrush(kText, br.put());
        const bool interactive = r.kind == K::Notif || r.kind == K::Group || r.kind == K::Collapse;
        if (hover && interactive) {
            br->SetColor(theme::Dark() ? ColorF(1, 1, 1, 0.06f) : ColorF(1, 1, 1, 0.35f));
            dc->FillRectangle(D2D1::RectF(0, 0, kW, r.h), br.get());
        }
        switch (r.kind) {
            case K::Collapse:
                br->SetColor(kText);
                dc->DrawText(L"Show less", 9, fPill.get(), D2D1::RectF(0, 0, kW, r.h), br.get());
                break;
            case K::More: {
                std::wstring t = std::to_wstring(r.count) + L" more";
                br->SetColor(kDim);
                dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), fPill.get(), D2D1::RectF(0, 0, kW, r.h), br.get());
                break;
            }
            case K::Empty:
                br->SetColor(kDim);
                dc->DrawText(L"No new notifications", 20, fPill.get(), D2D1::RectF(0, 0, kW, r.h), br.get());
                break;
            case K::Header: DrawHeader(dc.get(), br.get(), r, hover ? part : -1); break;
            case K::Calendar: DrawCalendar(dc.get(), br.get(), hover ? part : -1); break;
            default: DrawNotif(dc.get(), br.get(), r, hover, hover && part == 1, hover && part == 2); break;
        }
    } catch (...) {}
    interop->EndDraw();
}

void NotificationCenter::Impl::DrawHeader(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, Row const& r, int part) {
    br->SetColor(kText);
    dc->DrawText(L"Notifications", 13, fHead.get(), D2D1::RectF(kPad + 4, 0, kW - 100, r.h), br);
    if (!Visible()) return;
    const D2D1_ROUNDED_RECT btn = D2D1::RoundedRect(D2D1::RectF(kW - kPad - 58, 7, kW - kPad + 2, r.h - 7), 12, 12);
    br->SetColor(Wash(part == 2 ? 0.26f : 0.12f));
    dc->FillRoundedRectangle(btn, br);
    br->SetColor(kText);
    dc->DrawText(L"Clear", 5, fClear.get(), btn.rect, br);
}

void NotificationCenter::Impl::DrawNotif(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, Row const& r, bool hover, bool closeHover, bool clearHover) {
    // icon: the app's own icon, else a letter tile
    const D2D1_ROUNDED_RECT ir = D2D1::RoundedRect(D2D1::RectF(kPad, kHeaderY + 2, kPad + kIcon, kHeaderY + 2 + kIcon), 8, 8);
    auto ic = icons.find(r.n.appKey);
    bool drawn = false;
    if (ic != icons.end()) {
        winrt::com_ptr<ID2D1Bitmap> bmp;
        auto props = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(ic->second.w, ic->second.h), ic->second.px.data(), ic->second.w * 4, &props, bmp.put()))) {
            winrt::com_ptr<ID2D1BitmapBrush> bb;
            if (SUCCEEDED(dc->CreateBitmapBrush(bmp.get(), bb.put()))) {
                bb->SetInterpolationMode(D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                bb->SetTransform(D2D1::Matrix3x2F::Scale(kIcon / ic->second.w, kIcon / ic->second.h) * D2D1::Matrix3x2F::Translation(ir.rect.left, ir.rect.top));
                // plate behind transparent logos so they read on the dark glass
                br->SetColor(theme::Dark() ? ColorF(1, 1, 1, 0.10f) : ColorF(1, 1, 1, 0.85f));
                dc->FillRoundedRectangle(ir, br);
                dc->FillRoundedRectangle(ir, bb.get());
                drawn = true;
            }
        }
    }
    if (!drawn) {
        size_t hsh = std::hash<std::wstring>{}(r.n.appKey);
        br->SetColor(ColorF(0.25f + (hsh & 0xFF) / 700.0f, 0.3f + ((hsh >> 8) & 0xFF) / 700.0f, 0.5f + ((hsh >> 16) & 0xFF) / 600.0f, 1));
        dc->FillRoundedRectangle(ir, br);
        std::wstring letter = r.n.app.empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(towupper(r.n.app[0])));
        br->SetColor(theme::P().onAccent);
        dc->DrawText(letter.c_str(), 1, fPill.get(), ir.rect, br);
    }

    // header: app name, count chip, time (or the x while hovered)
    const float tx = kTextX, tw = kW - kTextX - kPad;
    std::wstring app = r.n.app.empty() ? L"Notification" : r.n.app;
    br->SetColor(kDim);
    float appW = std::min<float>(tw - 60, app.size() * 6.4f + 4);
    winrt::com_ptr<IDWriteTextLayout> lay;
    if (SUCCEEDED(dw->CreateTextLayout(app.c_str(), static_cast<UINT32>(app.size()), fApp.get(), tw - 70 - (r.clearBtn ? 54 : 0), 15, lay.put()))) {
        DWRITE_TEXT_METRICS tm{};
        lay->GetMetrics(&tm);
        appW = tm.widthIncludingTrailingWhitespace;
        dc->DrawTextLayout(D2D1::Point2F(tx, kHeaderY), lay.get(), br);
    }
    if (r.kind == K::Group && r.count > 1) {
        std::wstring cnt = std::to_wstring(r.count);
        float cw = 12.0f + 7.0f * cnt.size();
        D2D1_ROUNDED_RECT chip = D2D1::RoundedRect(D2D1::RectF(tx + appW + 6, kHeaderY + 0.5f, tx + appW + 6 + cw, kHeaderY + 15.5f), 7.5f, 7.5f);
        br->SetColor(Wash(0.20f));
        dc->FillRoundedRectangle(chip, br);
        br->SetColor(kText);
        dc->DrawText(cnt.c_str(), static_cast<UINT32>(cnt.size()), fCount.get(), chip.rect, br);
    }
    if (r.clearBtn) {  // the small text button, left of the time
        const D2D1_ROUNDED_RECT btn = D2D1::RoundedRect(D2D1::RectF(kW - kPad - 118, kHeaderY - 3, kW - kPad - 70, kHeaderY + 17), 10, 10);
        br->SetColor(Wash(clearHover ? 0.26f : 0.12f));
        dc->FillRoundedRectangle(btn, br);
        br->SetColor(kText);
        dc->DrawText(L"Clear", 5, fClear.get(), btn.rect, br);
    }
    if (hover) {
        const D2D1_POINT_2F ctr = D2D1::Point2F(kW - kPad - 9, kHeaderY + 8);
        br->SetColor(Wash(closeHover ? 0.32f : 0.16f));
        dc->FillEllipse(D2D1::Ellipse(ctr, 9, 9), br);
        br->SetColor(kText);
        dc->DrawLine(D2D1::Point2F(ctr.x - 3.2f, ctr.y - 3.2f), D2D1::Point2F(ctr.x + 3.2f, ctr.y + 3.2f), br, 1.4f);
        dc->DrawLine(D2D1::Point2F(ctr.x - 3.2f, ctr.y + 3.2f), D2D1::Point2F(ctr.x + 3.2f, ctr.y - 3.2f), br, 1.4f);
    } else {
        std::wstring t = Rel(r.n.ft);
        br->SetColor(kDim);
        dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), fTime.get(), D2D1::RectF(kW - kPad - 70, kHeaderY, kW - kPad, kHeaderY + 15), br);
    }

    std::wstring title = r.n.title.empty() ? L"New notification" : r.n.title;
    br->SetColor(kText);
    dc->DrawText(title.c_str(), static_cast<UINT32>(title.size()), fTitle.get(), D2D1::RectF(tx, kTitleY, tx + tw, kTitleY + 18), br);
    if (!r.n.body.empty()) {
        std::wstring body = r.n.body;
        int lines = 0;
        MeasureBody(body, tw, &lines);
        br->SetColor(kDim);
        dc->DrawText(body.c_str(), static_cast<UINT32>(body.size()), fBody.get(), D2D1::RectF(tx, kBodyY, tx + tw, kBodyY + kLine * 2 + 2), br);
    }
}

void NotificationCenter::Impl::DrawCalendar(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, int part) {
    wchar_t buf[96];
    SYSTEMTIME t = today;
    if (!attached) {  // big weekday + date (attached, the Control Center header shows them)
        br->SetColor(accent);
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, L"dddd", buf, 96, nullptr)) {
            CharUpperW(buf);
            dc->DrawText(buf, static_cast<UINT32>(wcslen(buf)), fCalDow.get(), D2D1::RectF(kPad, 14, kW - kPad, 32), br);
        }
        br->SetColor(kText);
        if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &t, L"d MMMM", buf, 96, nullptr))
            dc->DrawText(buf, static_cast<UINT32>(wcslen(buf)), fBig.get(), D2D1::RectF(kPad, 30, kW - kPad, 66), br);
    }

    // month nav
    SYSTEMTIME mo{};
    mo.wYear = static_cast<WORD>(calYear); mo.wMonth = static_cast<WORD>(calMonth); mo.wDay = 1;
    br->SetColor(part == 5 ? kText : theme::P().textSoft);
    if (GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &mo, L"MMMM yyyy", buf, 96, nullptr))
        dc->DrawText(buf, static_cast<UINT32>(wcslen(buf)), fNav.get(), D2D1::RectF(kPad, kCalHead, kW / 2 + 40, kCalHead + kCalNav), br);
    const float cy = kCalHead + kCalNav / 2;
    for (int d = 0; d < 2; ++d) {
        float cx = kW - kPad - 26 - 14 + d * 28 + 14 - 0;  // centres: prev at kW-kPad-40, next at kW-kPad-12
        cx = kW - kPad - 40 + d * 28;
        br->SetColor(theme::A(Wash(0.22f), part == 3 + d ? 1.0f : 0.0f));
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 12, 12), br);
        br->SetColor(kText);
        float dir = d ? 1.0f : -1.0f;
        dc->DrawLine(D2D1::Point2F(cx - dir * 2.0f, cy - 4.5f), D2D1::Point2F(cx + dir * 2.0f, cy), br, 1.6f);
        dc->DrawLine(D2D1::Point2F(cx + dir * 2.0f, cy), D2D1::Point2F(cx - dir * 2.0f, cy + 4.5f), br, 1.6f);
    }

    // weekday letters and the grid
    int fdVal = 0;
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, reinterpret_cast<LPWSTR>(&fdVal), 2);
    const int firstSun = (fdVal + 1) % 7;  // 0 = Monday in the locale, SYSTEMTIME counts from Sunday
    const float gx = kPad, cw = (kW - 2 * kPad) / 7, dowY = kCalHead + kCalNav + 4, gridY = dowY + kCalDow + 2;
    br->SetColor(kDim);
    for (int c = 0; c < 7; ++c) {
        int mon = ((firstSun + c) % 7 + 6) % 7;
        wchar_t nm[16] = L"";
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SABBREVDAYNAME1 + mon, nm, 16);
        UINT32 n = nm[0] ? 1 : 0;
        if (nm[0] >= 0xD800 && nm[0] < 0xDC00) n = 2;
        dc->DrawText(nm, n, fDow.get(), D2D1::RectF(gx + c * cw, dowY, gx + (c + 1) * cw, dowY + kCalDow), br);
    }
    FILETIME ft;
    SYSTEMTIME first{};
    SystemTimeToFileTime(&mo, &ft);
    FileTimeToSystemTime(&ft, &first);
    const int offset = (first.wDayOfWeek - firstSun + 7) % 7;
    const int dim = DaysIn(calYear, calMonth);
    const int prevDim = calMonth == 1 ? 31 : DaysIn(calYear, calMonth - 1);
    for (int i = 0; i < 42; ++i) {
        int day = i - offset + 1;
        bool inMonth = day >= 1 && day <= dim;
        if (day < 1) day += prevDim;
        else if (day > dim) day -= dim;
        const float cx = gx + (i % 7) * cw + cw / 2, cyy = gridY + (i / 7) * kCalRow + kCalRow / 2;
        const bool isToday = inMonth && calYear == today.wYear && calMonth == today.wMonth && day == today.wDay;
        if (isToday) {
            br->SetColor(accent);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cyy), std::min(12.0f, kCalRow / 2 - 1), std::min(12.0f, kCalRow / 2 - 1)), br);
        }
        std::wstring ds = std::to_wstring(day);
        br->SetColor(isToday ? theme::P().onAccent : inMonth ? kText : theme::P().faint);
        dc->DrawText(ds.c_str(), static_cast<UINT32>(ds.size()), isToday ? fDayB.get() : fDay.get(),
                     D2D1::RectF(cx - cw / 2, cyy - kCalRow / 2, cx + cw / 2, cyy + kCalRow / 2), br);
    }
}

// Each card slides 12 px in from the right and fades, 25 ms after the one before it. Compositor only.
void NotificationCenter::Impl::AnimateIn(Card& c, int order) {
    const auto delay = std::chrono::milliseconds(25 * order);
    auto ease = comp.CreateCubicBezierEasingFunction({0.2f, 0.8f}, {0.2f, 1.0f});
    c.outer.Opacity(0);
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, 1.0f);
    fade.Duration(200ms);
    fade.DelayTime(delay);
    fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
    c.outer.StartAnimation(L"Opacity", fade);
    auto slide = comp.CreateScalarKeyFrameAnimation();
    slide.InsertKeyFrame(0.0f, 12.0f * s);
    slide.InsertKeyFrame(1.0f, 0.0f, ease);
    slide.Duration(300ms);
    slide.DelayTime(delay);
    slide.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
    c.outer.StartAnimation(L"Offset.X", slide);
}

void NotificationCenter::Impl::AnimateOut(Card& c) {
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, 0.0f);
    fade.Duration(130ms);
    c.outer.StartAnimation(L"Opacity", fade);
    auto scale = comp.CreateVector3KeyFrameAnimation();
    scale.InsertKeyFrame(1.0f, {0.96f, 0.96f, 1.0f});
    scale.Duration(130ms);
    c.outer.StartAnimation(L"Scale", scale);
}

// Diffs the wanted rows against the live cards: new ones animate in (staggered), gone ones fade out,
// survivors glide to their new slot.
void NotificationCenter::Impl::Update(bool firstShow) {
    if (st != St::Open || !root) return;
    GetLocalTime(&today);
    auto src = BuildRows();

    std::vector<Row> rows;
    float y = 0;
    auto add = [&](Row r, float h, float extra = 0) {
        r.h = h;
        r.y = y;
        y += h + extra + kGap;
        rows.push_back(std::move(r));
    };
    if (!attached) {
        Row r;
        r.key = L"hdr";
        r.kind = K::Header;
        r.count = Visible();
        add(std::move(r), kHeadH);
    }
    const float cardsAvail = availH - (attached ? 0 : kHeadH + kGap) - (kCalH + kGap);
    if (haveData && src.empty()) {
        Row r;
        r.key = L"empty";
        r.kind = K::Empty;
        add(std::move(r), attached ? 36.0f : kEmptyH);
    }
    // heights first, then keep the prefix that fits under the card limit
    struct Sized { Row r; float h; };
    std::vector<Sized> sized;
    for (auto& r : src) {
        float h;
        if (r.kind == K::Collapse) h = kCollapseH;
        else {
            int lines = 0;
            std::wstring b = r.n.body;
            MeasureBody(b, kW - kTextX - kPad, &lines);
            h = lines ? std::max(kMinH, kBodyY + kLine * lines + 14) : std::max(kMinH, kTitleY + 18 + 14);
        }
        sized.push_back({std::move(r), h});
    }
    size_t keep = 0;
    float used = 0;
    int cardsShown = 0;
    for (auto& z : sized) {
        const bool isCard = z.r.kind != K::Collapse;
        float use = z.h + (z.r.kind == K::Group ? kPeek : 0) + kGap;
        if ((isCard && cardsShown >= (attached ? kMaxCardsAttached : kMaxCards)) || used + use > cardsAvail) break;
        used += use;
        cardsShown += isCard;
        ++keep;
    }
    // room for the "N more" pill
    if (keep < sized.size() && !attached)
        while (keep > 1 && used + kMoreH + kGap > cardsAvail) {
            --keep;
            used -= sized[keep].h + (sized[keep].r.kind == K::Group ? kPeek : 0) + kGap;
        }
    int more = 0;
    for (size_t i = keep; i < sized.size(); ++i)
        if (sized[i].r.kind == K::Notif) more += 1;
        else if (sized[i].r.kind == K::Group) more += sized[i].r.count;
    if (attached && keep && sized[0].r.kind != K::Collapse) sized[0].r.clearBtn = true;
    for (size_t i = 0; i < keep; ++i) {
        const float extra = sized[i].r.kind == K::Group ? kPeek : 0;
        add(std::move(sized[i].r), sized[i].h, extra);
    }
    if (more && (!attached || used + kMoreH + kGap <= cardsAvail)) {
        Row r;
        r.key = L"more";
        r.kind = K::More;
        r.count = more;
        add(std::move(r), kMoreH);
    }
    {
        Row r;
        r.key = L"cal";
        r.kind = K::Calendar;
        add(std::move(r), kCalH);
    }
    totalH = y - kGap;

    std::vector<Card> next;
    int order = 0;
    for (auto& r : rows) {
        std::wstring sig = r.n.title + L"\x1f" + r.n.body + L"\x1f" + Rel(r.n.ft) + L"\x1f" + std::to_wstring(r.count) + L"\x1f" +
                           std::to_wstring(r.h) + (r.kind == K::Group ? L"g" : L"") + (r.clearBtn ? L"C" : L"") + (icons.count(r.n.appKey) ? L"i" : L"") +
                           (r.kind == K::Calendar ? std::to_wstring(today.wYear * 10000 + today.wMonth * 100 + today.wDay) : L"");
        auto it = std::find_if(cards.begin(), cards.end(), [&](Card const& c) { return c.row.key == r.key && !c.gone; });
        if (it != cards.end() && it->row.h == r.h && it->row.kind == r.kind) {
            Card c = std::move(*it);
            cards.erase(it);
            bool moved = c.row.y != r.y;
            c.row = r;
            if (c.sig != sig) { c.sig = sig; Draw(c, false, -1); }
            if (moved) {
                auto glide = comp.CreateScalarKeyFrameAnimation();
                glide.InsertKeyFrame(1.0f, r.y * s, comp.CreateCubicBezierEasingFunction({0.2f, 0.8f}, {0.2f, 1.0f}));
                glide.Duration(260ms);
                c.outer.StartAnimation(L"Offset.Y", glide);
            }
            next.push_back(std::move(c));
        } else {
            Card c;
            c.row = r;
            c.sig = sig;
            CreateCard(c);
            c.outer.Offset({0, r.y * s, 0});
            panelVis.Children().InsertAtTop(c.outer);
            if (!(attached && firstShow)) AnimateIn(c, order);  // attached: the whole column moves as one
            ++order;
            next.push_back(std::move(c));
        }
    }
    // whatever is left disappears
    for (auto& c : cards) {
        c.gone = true;
        AnimateOut(c);
        dying.push_back(std::move(c));
    }
    cards = std::move(next);
    hoverCard = -1;
    if (!dying.empty()) SetTimer(hwnd, kTimerGone, 170, nullptr);
    Place();
}

// The window is content-sized, with motion::kPad of empty margin around the visual; it stays tall
// enough for fading cards and shrinks once they are gone.
void NotificationCenter::Impl::Place() {
    if (!hwnd) return;
    float h = totalH;
    if (!dying.empty()) h = std::max(h, winH);
    h = std::min(h, availH);
    winH = h;
    const int wPx = static_cast<int>(std::ceil((kW + 2 * motion::kPad) * s));
    const int hPx = std::max(1, static_cast<int>(std::ceil((h + 2 * motion::kPad) * s)));
    int x = work.right - wPx + static_cast<int>((motion::kPad - kEdge) * s);
    int yy = work.top + static_cast<int>((kEdge - motion::kPad) * s);
    if (attached) {  // directly under the Control Center, same left edge
        const int pad = static_cast<int>(std::lround(motion::kPad * s));
        x = anchorRc.left - pad;
        yy = anchorRc.bottom + static_cast<int>(std::lround(kAttachGap * s)) - pad;
    }
    SetWindowPos(hwnd, HWND_TOPMOST, x, yy, wPx, hPx, SWP_NOACTIVATE);
}

// The panel takes focus so Esc works and clicking elsewhere closes it (same as the Control Center).
void NotificationCenter::Impl::Focus() {
    DWORD me = GetCurrentThreadId(), fg = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    bool attached = fg && fg != me && AttachThreadInput(me, fg, TRUE);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    if (attached) AttachThreadInput(me, fg, FALSE);
}

// Monitor, scale, width, available height and calendar size. Attached, everything follows the Control Center.
void NotificationCenter::Impl::ApplyGeometry() {
    HMONITOR hm;
    if (attached) hm = MonitorFromRect(&anchorRc, MONITOR_DEFAULTTOPRIMARY);
    else hm = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(hm, &mi);
    work = mi.rcWork;
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
    s = dx / 96.0f;
    if (!attached) {
        kW = kWStd;
        kCalHead = kCalHeadStd;
        kCalRow = kCalRowStd;
        kCalH = kCalHStd;
        availH = (work.bottom - work.top) / s - 2 * kEdge;
        return;
    }
    kW = (anchorRc.right - anchorRc.left) / s;
    const float top = anchorRc.bottom + kAttachGap * s;
    availH = std::max(120.0f, (mi.rcMonitor.bottom - top) / s - kAttachMargin);
    // Compact calendar: just the month row, the weekday letters and the grid. Shorter rows only if even
    // the calendar alone would not fit (cards are dropped first, see Update).
    kCalHead = 4;
    auto calH = [&](float row) { return kCalHead + kCalNav + 4 + kCalDow + 2 + 6 * row + 8; };
    kCalRow = availH < calH(24) ? 20.0f : 24.0f;
    kCalH = calH(kCalRow);
}

void NotificationCenter::Impl::Open() {
    if (st == St::Open) return;
    if (st == St::Closing) Teardown();
    visible = true;
    try {
        ApplyGeometry();
        try {
            auto c = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(winrt::Windows::UI::ViewManagement::UIColorType::Accent);
            accent = ColorF(c.R / 255.0f, c.G / 255.0f, c.B / 255.0f, 1);
        } catch (...) {}
        GetLocalTime(&today);
        calYear = today.wYear;
        calMonth = today.wMonth;
        BuildFonts();
        hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"Halo Notification Center", WS_POPUP,
                               work.right - 1, work.top, 1, 1, nullptr, nullptr, inst, this);
        if (!hwnd) { visible = false; return; }
        BOOL on = TRUE;
        DwmSetWindowAttribute(hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));
        winrt::check_hresult(comp.as<ICompositorDesktopInterop>()->CreateDesktopWindowTarget(hwnd, TRUE, winrt::put_abi(target)));
        root = comp.CreateContainerVisual();
        root.RelativeSizeAdjustment({1, 1});
        target.Root(root);
        panelVis = comp.CreateContainerVisual();
        panelVis.Size({std::ceil(kW * s), 1});
        const float3 rest{motion::kPad * s, motion::kPad * s, 0};
        panelVis.Offset(rest);
        root.Children().InsertAtTop(panelVis);
        try { noise = NoiseBrush(static_cast<int>(std::ceil(kW * s)), 256); } catch (...) { noise = nullptr; }
        st = St::Open;
        winH = 0;
        Update(true);
        ShowWindow(hwnd, attached ? SW_SHOWNOACTIVATE : SW_SHOW);  // attached: the Control Center keeps the focus
        if (!attached) Focus();
        motion::In(comp, panelVis, rest, Anchor(), Travel());
        SetEvent(evReq);
        svc::Log(std::wstring(L"notifications: open") + (attached ? L" (attached)" : L"") + L" availH=" + std::to_wstring(static_cast<int>(availH)) +
                 L" totalH=" + std::to_wstring(static_cast<int>(totalH)));
    } catch (winrt::hresult_error const& e) {
        svc::Log(L"notifications: open failed: " + std::wstring(e.message().c_str()));
        Teardown();
    }
}

void NotificationCenter::Impl::Close() {
    visible = false;
    if (st != St::Open) return;
    st = St::Closing;
    motion::Out(comp, panelVis, {motion::kPad * s, motion::kPad * s, 0}, Anchor(), Travel());
    SetTimer(hwnd, kTimerClose, motion::kOutMs + 10, nullptr);
}

void NotificationCenter::Impl::Teardown() {
    visible = false;
    if (hwnd) { KillTimer(hwnd, kTimerGone); KillTimer(hwnd, kTimerClose); }
    cards.clear();
    dying.clear();
    panelVis = nullptr;
    root = nullptr;
    target = nullptr;
    noise = nullptr;
    if (hwnd) DestroyWindow(hwnd);
    hwnd = nullptr;
    hoverCard = -1;
    tracking = false;
    st = St::Closed;
}

// ---------------------------------------------------------------------------------------------
// public

NotificationCenter::NotificationCenter(HINSTANCE inst, HWND, wuc::Compositor comp, wuc::CompositionGraphicsDevice gdev)
    : m(std::make_unique<Impl>(this, inst, std::move(comp), std::move(gdev))) {
    m->Start();
}
NotificationCenter::~NotificationCenter() { m->Stop(); }
void NotificationCenter::Toggle() { IsOpen() ? Close() : Open(); }
void NotificationCenter::Open() { m->Open(); }
void NotificationCenter::Close() { m->Close(); }
bool NotificationCenter::IsOpen() const { return m->st == Impl::St::Open; }
HWND NotificationCenter::Hwnd() const { return m->st == Impl::St::Closed ? nullptr : m->hwnd; }
// Attach under the Control Center panel (physical px); an empty rect detaches. Re-lays out while open.
void NotificationCenter::SetAnchor(RECT ccPanel) {
    m->attached = ccPanel.right > ccPanel.left;
    m->anchorRc = ccPanel;
    if (m->st == Impl::St::Open && m->attached) {
        m->ApplyGeometry();
        m->Update(false);
    }
}

}  // namespace halo
