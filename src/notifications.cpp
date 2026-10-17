#include "notifications.h"

#include <d2d1_1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>
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
#include "services.h"

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

namespace halo {
namespace {

// Logical px; everything is scaled by the monitor DPI.
constexpr float kPad = 14, kIcon = 36, kTextX = kPad + kIcon + 10, kRadius = 18, kGap = 8, kBelowPanel = 10, kPeek = 10;
constexpr float kHeaderY = 12, kTitleY = 29, kBodyY = 47, kLine = 15, kMinH = 62, kCollapseH = 28;
constexpr int kMaxRows = 7;
constexpr UINT kMsgData = WM_APP + 1;
constexpr UINT_PTR kTimerGone = 1, kTimerClose = 2;
constexpr wchar_t kClass[] = L"HaloNotifications", kMsgClass[] = L"HaloNotificationsMsg";

const D2D1_COLOR_F kText = ColorF(1, 1, 1, 0.94f), kDim = ColorF(1, 1, 1, 0.58f);

struct Notif {
    UINT id = 0;
    std::wstring appKey, aumid, app, title, body;
    LONGLONG ft = 0;  // FILETIME of creation
};
struct Snapshot {
    std::vector<Notif> items;
    std::map<std::wstring, std::vector<uint8_t>> logos;  // appKey -> encoded image, first sight only
    bool ok = false;
    std::wstring error;
};

// What one card shows.
struct Row {
    std::wstring key;
    bool collapse = false;  // the "Show less" pill under an expanded group
    bool group = false;     // collapsed group head (stack peeks under it)
    int count = 1;
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
    if (sec < 3600) return std::to_wstring(sec / 60) + L"m ago";
    if (sec < 86400) return std::to_wstring(sec / 3600) + L"h ago";
    return std::to_wstring(sec / 86400) + L"d ago";
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

struct IconPx {
    UINT w = 0, h = 0;
    std::vector<uint32_t> px;
};

}  // namespace

struct NotificationStack::Impl {
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

    // ---- window + cards (exist only while shown) ----
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
    float s = 1, W = 336;
    RECT panel{};
    wuc::Desktop::DesktopWindowTarget target{nullptr};
    wuc::ContainerVisual root{nullptr};
    wuc::CompositionBrush noise{nullptr};
    std::vector<Card> cards;
    std::vector<Card> dying;
    float totalH = 0;
    int hoverCard = -1;
    bool hoverClose = false, tracking = false;

    winrt::com_ptr<IDWriteFactory> dw;
    winrt::com_ptr<IDWriteTextFormat> fApp, fTime, fTitle, fBody, fPill, fCount;

    Impl(HINSTANCE i, wuc::Compositor c, wuc::CompositionGraphicsDevice g) : inst(i), comp(std::move(c)), gdev(std::move(g)) {}

    void Start();
    void Stop();
    void Worker();
    Snapshot Query(std::set<std::wstring>& logoDone);
    void OnSnapshot(Snapshot& snap);

    void Show(RECT p);
    void Hide();
    void Teardown();
    void BuildFonts();
    std::vector<Row> BuildRows();
    void Update(bool firstShow);
    void CreateCard(Card& c);
    void Draw(Card& c, bool hover, bool closeHover);
    void Place();
    void AnimateIn(Card& c, int order);
    void AnimateOut(Card& c);
    int CardAt(float x, float y, bool* onClose);
    void Dismiss(Row const& r);
    void Activate(Row const& r);
    float MeasureBody(std::wstring& body, float width, int* lines);
    wuc::CompositionBrush NoiseBrush(int w, int h);

    LRESULT Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    static LRESULT CALLBACK Thunk(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK MsgThunk(HWND, UINT, WPARAM, LPARAM);
};

// ---------------------------------------------------------------------------------------------
// worker: listener queries, woken by a change notification on the notification database folder

Snapshot NotificationStack::Impl::Query(std::set<std::wstring>& logoDone) {
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
            auto const& k = snap.items.back().appKey;
            if (!logoDone.count(k)) {
                logoDone.insert(k);
                try {
                                    auto ref = info.DisplayInfo().GetLogo({72, 72});
                    if (!ref) continue;  // some apps (Windows Security) have no logo: null, not an error
                    auto stream = ref.OpenReadAsync().get();
                    winrt::Windows::Storage::Streams::DataReader rd(stream);
                    auto size = static_cast<uint32_t>(stream.Size());
                    if (size && size < (4u << 20)) {
                        rd.LoadAsync(size).get();
                        std::vector<uint8_t> bytes(size);
                        rd.ReadBytes(bytes);
                        snap.logos[k] = std::move(bytes);
                    }
                } catch (...) {}
            }
        }
        snap.ok = true;
    } catch (winrt::hresult_error const& e) {
        snap.error = L"0x" + std::to_wstring(static_cast<unsigned>(e.code().value)) + L" " + e.message().c_str();
    }
    return snap;
}

void NotificationStack::Impl::Worker() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    wchar_t local[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring dir = std::wstring(local) + L"\\Microsoft\\Windows\\Notifications";
    HANDLE watch = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (watch == INVALID_HANDLE_VALUE) { svc::Log(L"notifications: cannot watch " + dir); watch = nullptr; }
    std::set<std::wstring> logoDone;
    bool prefetch = true;  // one read at startup so the first open is instant
    HANDLE h[3] = {evStop, evReq, watch};
    for (;;) {
        DWORD r = prefetch ? WAIT_OBJECT_0 + 1 : WaitForMultipleObjects(watch ? 3 : 2, h, FALSE, INFINITE);
        bool pf = prefetch;
        prefetch = false;
        if (r == WAIT_OBJECT_0) break;
        if (r == WAIT_OBJECT_0 + 2) {
            FindNextChangeNotification(watch);
            if (!visible) continue;  // the next Show asks for a fresh read anyway
            // The database writes in bursts; settle first.
            for (int i = 0; i < 10 && WaitForSingleObject(watch, 250) == WAIT_OBJECT_0; ++i) FindNextChangeNotification(watch);
            svc::Log(L"notifications: database changed");
        }
        if (!visible) continue;
            auto* snap = new Snapshot(Query(logoDone));
        if (!msgWnd || !PostMessageW(msgWnd, kMsgData, 0, reinterpret_cast<LPARAM>(snap))) delete snap;
    }
    if (watch) FindCloseChangeNotification(watch);
}

void NotificationStack::Impl::Start() {
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

void NotificationStack::Impl::Stop() {
    Teardown();
    SetEvent(evStop);
    if (worker.joinable()) worker.join();
    HWND mw = msgWnd;
    msgWnd = nullptr;
    if (mw) DestroyWindow(mw);
    CloseHandle(evStop);
    CloseHandle(evReq);
}

LRESULT CALLBACK NotificationStack::Impl::MsgThunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == kMsgData) {
        std::unique_ptr<Snapshot> snap(reinterpret_cast<Snapshot*>(l));
        if (self) self->OnSnapshot(*snap);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void NotificationStack::Impl::OnSnapshot(Snapshot& snap) {
    if (!snap.ok) { svc::Log(L"notifications: read failed: " + snap.error); return; }
    for (auto& [k, bytes] : snap.logos) {
        IconPx ic;
        if (Decode(bytes, ic.w, ic.h, ic.px)) icons[k] = std::move(ic);
    }
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

std::vector<Row> NotificationStack::Impl::BuildRows() {
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
                r.collapse = true;
                r.groupKey = k;
                rows.push_back(std::move(r));
            }
        } else {
            Row r;
            r.key = L"g" + k;
            r.group = true;
            r.groupKey = k;
            r.count = static_cast<int>(g.size());
            for (auto* n : g) r.ids.push_back(n->id);
            r.n = *g[0];
            rows.push_back(std::move(r));
        }
    }
    return rows;
}

float NotificationStack::Impl::MeasureBody(std::wstring& body, float width, int* lines) {
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

// ---------------------------------------------------------------------------------------------
// window

LRESULT CALLBACK NotificationStack::Impl::Thunk(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return self ? self->Proc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

int NotificationStack::Impl::CardAt(float x, float y, bool* onClose) {
    if (onClose) *onClose = false;
    for (size_t i = 0; i < cards.size(); ++i) {
        auto const& r = cards[i].row;
        if (x < 0 || x >= W || y < r.y || y >= r.y + r.h) continue;
        if (onClose && !r.collapse) {
            float cx = W - kPad - 9, cy = r.y + 20;
            *onClose = (x - cx) * (x - cx) + (y - cy) * (y - cy) <= 11 * 11;
        }
        return static_cast<int>(i);
    }
    return -1;
}

LRESULT NotificationStack::Impl::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    const float x = static_cast<short>(LOWORD(l)) / s, y = static_cast<short>(HIWORD(l)) / s;
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // never steal focus: the Control Center closes when it loses it
        case WM_NCHITTEST: {
            POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
            ScreenToClient(h, &p);
            return CardAt(p.x / s, p.y / s, nullptr) >= 0 ? HTCLIENT : HTTRANSPARENT;
        }
        case WM_MOUSEMOVE: {
            if (!tracking && !_wgetenv(L"HALO_TEST")) {  // test hook: posted moves must not be undone by a real WM_MOUSELEAVE
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                tracking = TrackMouseEvent(&tme);
            }
            bool cl = false;
            int i = CardAt(x, y, &cl);
            if (i != hoverCard || cl != hoverClose) {
                int old = hoverCard;
                hoverCard = i;
                hoverClose = cl;
                if (old >= 0 && old < static_cast<int>(cards.size())) Draw(cards[old], false, false);
                if (i >= 0) Draw(cards[i], true, cl);
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            tracking = false;
            int old = hoverCard;
            hoverCard = -1;
            hoverClose = false;
            if (old >= 0 && old < static_cast<int>(cards.size())) Draw(cards[old], false, false);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (st != St::Open) return 0;
            bool cl = false;
            int i = CardAt(x, y, &cl);
            if (i < 0) return 0;
            Row r = cards[i].row;  // copy: Update() rebuilds the cards
            if (r.collapse) { expanded.erase(r.groupKey); Update(false); }
            else if (cl) Dismiss(r);
            else if (r.group) { expanded.insert(r.groupKey); Update(false); }
            else Activate(r);
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
    }
    return DefWindowProcW(h, m, w, l);
}

void NotificationStack::Impl::Dismiss(Row const& r) {
    for (UINT id : r.ids) {
        hidden.insert(id);
        if (dry) { svc::Log(L"notifications: DRY RUN, would remove id " + std::to_wstring(id)); continue; }
        try { wun::Management::UserNotificationListener::Current().RemoveNotification(id); }
        catch (winrt::hresult_error const& e) { svc::Log(L"notifications: RemoveNotification failed, hidden locally: " + std::wstring(e.message().c_str())); }
    }
    hoverCard = -1;
    Update(false);
}

void NotificationStack::Impl::Activate(Row const& r) {
    if (dry) { svc::Log(L"notifications: DRY RUN, would open " + r.n.aumid + L" and remove id " + std::to_wstring(r.n.id)); return; }
    if (!r.n.aumid.empty()) {
        auto res = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + r.n.aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        svc::Log(L"notifications: open " + r.n.aumid + L" -> " + std::to_wstring(res));
    }
    Dismiss(r);  // Windows removes a toast once it has been clicked
}

void NotificationStack::Impl::BuildFonts() {
    if (dw) return;
    winrt::check_hresult(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.put())));
    auto mk = [&](winrt::com_ptr<IDWriteTextFormat>& out, DWRITE_FONT_WEIGHT wt, float size, DWRITE_TEXT_ALIGNMENT al, bool wrap) {
        winrt::check_hresult(dw->CreateTextFormat(L"Segoe UI Variable Text", nullptr, wt, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", out.put()));
        out->SetTextAlignment(al);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!wrap) {
            winrt::com_ptr<IDWriteInlineObject> sign;
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(out.get(), sign.put()))) out->SetTrimming(&trim, sign.get());
        }
    };
    const auto SB = DWRITE_FONT_WEIGHT_SEMI_BOLD, R = DWRITE_FONT_WEIGHT_NORMAL;
    mk(fApp, SB, 11, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    mk(fTime, R, 11, DWRITE_TEXT_ALIGNMENT_TRAILING, false);
    mk(fTitle, SB, 12.5f, DWRITE_TEXT_ALIGNMENT_LEADING, false);
    mk(fBody, R, 11.5f, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    mk(fPill, SB, 11.5f, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    mk(fCount, SB, 10.5f, DWRITE_TEXT_ALIGNMENT_CENTER, false);
    fPill->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    fCount->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

wuc::CompositionBrush NotificationStack::Impl::NoiseBrush(int w, int h) {
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
void NotificationStack::Impl::CreateCard(Card& c) {
    const float wPx = std::ceil(W * s), hPx = std::ceil(c.row.h * s);
    c.outer = comp.CreateContainerVisual();
    c.outer.Size({wPx, hPx + (c.row.group ? kPeek * s : 0)});
    c.outer.CenterPoint({wPx / 2, 0, 0});

    if (c.row.group) {  // the rest of the stack peeking out from under the card
        for (int i = 2; i >= 1; --i) {
            auto peek = comp.CreateSpriteVisual();
            float inset = (7.0f * i) * s;
            peek.Size({wPx - 2 * inset, 20 * s});
            peek.Offset({inset, hPx + (i == 1 ? 5.0f : 10.0f) * s - 20 * s, 0});
            peek.Brush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(i == 1 ? 0x70 : 0x48, 0x16, 0x16, 0x1A)));
            auto g = comp.CreateRoundedRectangleGeometry();
            g.CornerRadius({kRadius * s, kRadius * s});
            g.Size(peek.Size());
            peek.Clip(comp.CreateGeometricClip(g));
            c.outer.Children().InsertAtTop(peek);
        }
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
    tint.Brush(comp.CreateColorBrush(winrt::Windows::UI::ColorHelper::FromArgb(0x80, 0x16, 0x16, 0x1A)));
    c.vis.Children().InsertAtTop(tint);

    if (noise) {
        auto n = comp.CreateSpriteVisual();
        n.RelativeSizeAdjustment({1, 1});
        n.Opacity(0.025f);
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
    const float rad = c.row.collapse ? c.row.h / 2 * s : kRadius * s;
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
    auto sprite = comp.CreateSpriteShape(c.rim);
    sprite.StrokeBrush(gradient);
    sprite.StrokeThickness(t);
    auto rimVisual = comp.CreateShapeVisual();
    rimVisual.RelativeSizeAdjustment({1, 1});
    rimVisual.Shapes().Append(sprite);
    c.vis.Children().InsertAtTop(rimVisual);

    c.clip = comp.CreateRoundedRectangleGeometry();
    c.clip.CornerRadius({rad, rad});
    c.clip.Size({wPx, hPx});
    c.vis.Clip(comp.CreateGeometricClip(c.clip));
    Draw(c, false, false);
}

void NotificationStack::Impl::Draw(Card& c, bool hover, bool closeHover) {
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
        if (hover) {
            br->SetColor(ColorF(1, 1, 1, 0.06f));
            dc->FillRectangle(D2D1::RectF(0, 0, W, r.h), br.get());
        }
        if (r.collapse) {
            br->SetColor(kText);
            dc->DrawText(L"Show less", 9, fPill.get(), D2D1::RectF(0, 0, W, r.h), br.get());
        } else {
            // icon: the app's own logo if Windows gave one, else a letter tile
            const D2D1_ROUNDED_RECT ir = D2D1::RoundedRect(D2D1::RectF(kPad, kHeaderY + 2, kPad + kIcon, kHeaderY + 2 + kIcon), 9, 9);
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
                        br->SetColor(ColorF(1, 1, 1, 0.14f));
                        dc->FillRoundedRectangle(ir, br.get());
                        dc->FillRoundedRectangle(ir, bb.get());
                        drawn = true;
                    }
                }
            }
            if (!drawn) {
                size_t hsh = std::hash<std::wstring>{}(r.n.appKey);
                br->SetColor(ColorF(0.25f + (hsh & 0xFF) / 700.0f, 0.3f + ((hsh >> 8) & 0xFF) / 700.0f, 0.5f + ((hsh >> 16) & 0xFF) / 600.0f, 1));
                dc->FillRoundedRectangle(ir, br.get());
                std::wstring letter = r.n.app.empty() ? L"?" : std::wstring(1, static_cast<wchar_t>(towupper(r.n.app[0])));
                br->SetColor(kText);
                dc->DrawText(letter.c_str(), 1, fPill.get(), ir.rect, br.get());
            }

            // header: app name, count chip, time (or the x while hovered)
            const float tx = kTextX, tw = W - kTextX - kPad;
            std::wstring app = r.n.app.empty() ? L"Notification" : r.n.app;
            br->SetColor(kDim);
            float appW = std::min<float>(tw - 60, app.size() * 6.4f + 4);
            winrt::com_ptr<IDWriteTextLayout> lay;
            if (SUCCEEDED(dw->CreateTextLayout(app.c_str(), static_cast<UINT32>(app.size()), fApp.get(), tw - 70, 15, lay.put()))) {
                DWRITE_TEXT_METRICS tm{};
                lay->GetMetrics(&tm);
                appW = tm.widthIncludingTrailingWhitespace;
                dc->DrawTextLayout(D2D1::Point2F(tx, kHeaderY), lay.get(), br.get());
            }
            if (r.group && r.count > 1) {
                std::wstring cnt = std::to_wstring(r.count);
                float cw = 12.0f + 7.0f * cnt.size();
                D2D1_ROUNDED_RECT chip = D2D1::RoundedRect(D2D1::RectF(tx + appW + 6, kHeaderY + 0.5f, tx + appW + 6 + cw, kHeaderY + 15.5f), 7.5f, 7.5f);
                br->SetColor(ColorF(1, 1, 1, 0.20f));
                dc->FillRoundedRectangle(chip, br.get());
                br->SetColor(kText);
                dc->DrawText(cnt.c_str(), static_cast<UINT32>(cnt.size()), fCount.get(), chip.rect, br.get());
            }
            if (hover) {
                const D2D1_POINT_2F ctr = D2D1::Point2F(W - kPad - 9, kHeaderY + 8);
                br->SetColor(ColorF(1, 1, 1, closeHover ? 0.32f : 0.16f));
                dc->FillEllipse(D2D1::Ellipse(ctr, 9, 9), br.get());
                br->SetColor(kText);
                dc->DrawLine(D2D1::Point2F(ctr.x - 3.2f, ctr.y - 3.2f), D2D1::Point2F(ctr.x + 3.2f, ctr.y + 3.2f), br.get(), 1.4f);
                dc->DrawLine(D2D1::Point2F(ctr.x - 3.2f, ctr.y + 3.2f), D2D1::Point2F(ctr.x + 3.2f, ctr.y - 3.2f), br.get(), 1.4f);
            } else {
                std::wstring t = Rel(r.n.ft);
                br->SetColor(kDim);
                dc->DrawText(t.c_str(), static_cast<UINT32>(t.size()), fTime.get(), D2D1::RectF(W - kPad - 70, kHeaderY, W - kPad, kHeaderY + 15), br.get());
            }

            std::wstring title = r.n.title.empty() ? L"New notification" : r.n.title;
            br->SetColor(kText);
            dc->DrawText(title.c_str(), static_cast<UINT32>(title.size()), fTitle.get(), D2D1::RectF(tx, kTitleY, tx + tw, kTitleY + 18), br.get());
            if (!r.n.body.empty()) {
                std::wstring body = r.n.body;
                int lines = 0;
                MeasureBody(body, tw, &lines);
                br->SetColor(kDim);
                dc->DrawText(body.c_str(), static_cast<UINT32>(body.size()), fBody.get(), D2D1::RectF(tx, kBodyY, tx + tw, kBodyY + kLine * 2 + 2), br.get());
            }
        }
    } catch (...) {}
    interop->EndDraw();
}

void NotificationStack::Impl::AnimateIn(Card& c, int order) {
    const auto delay = std::chrono::milliseconds(60 + 55 * order);
    c.outer.Opacity(0);
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, 1.0f);
    fade.Duration(180ms);
    fade.DelayTime(delay);
    fade.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
    c.outer.StartAnimation(L"Opacity", fade);
    auto slide = comp.CreateSpringScalarAnimation();
    slide.InitialValue((c.row.y - 14.0f) * s);
    slide.FinalValue(c.row.y * s);
    slide.DampingRatio(0.78f);
    slide.Period(50ms);
    slide.DelayTime(delay);
    slide.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
    c.outer.StartAnimation(L"Offset.Y", slide);
    auto scale = comp.CreateSpringVector3Animation();
    scale.InitialValue(winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::Numerics::float3>(
        winrt::Windows::Foundation::Numerics::float3{0.96f, 0.96f, 1.0f}));
    scale.FinalValue(winrt::Windows::Foundation::IReference<winrt::Windows::Foundation::Numerics::float3>(
        winrt::Windows::Foundation::Numerics::float3{1.0f, 1.0f, 1.0f}));
    scale.DampingRatio(0.78f);
    scale.Period(50ms);
    scale.DelayTime(delay);
    scale.DelayBehavior(wuc::AnimationDelayBehavior::SetInitialValueBeforeDelay);
    c.outer.StartAnimation(L"Scale", scale);
}

void NotificationStack::Impl::AnimateOut(Card& c) {
    auto fade = comp.CreateScalarKeyFrameAnimation();
    fade.InsertKeyFrame(1.0f, 0.0f);
    fade.Duration(130ms);
    c.outer.StartAnimation(L"Opacity", fade);
    auto scale = comp.CreateVector3KeyFrameAnimation();
    scale.InsertKeyFrame(1.0f, {0.96f, 0.96f, 1.0f});
    scale.Duration(130ms);
    c.outer.StartAnimation(L"Scale", scale);
}

// Diffs the wanted rows against the live cards: new ones spring in (staggered), gone ones fade out,
// survivors glide to their new slot.
void NotificationStack::Impl::Update(bool firstShow) {
    if (st != St::Open || !root) return;
    auto rows = BuildRows();

    // geometry, capped to what fits under the panel
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&panel, MONITOR_DEFAULTTONEAREST), &mi);
    const float avail = (mi.rcMonitor.bottom - panel.bottom) / s - kBelowPanel - 8;
    float y = 0;
    size_t keep = 0;
    for (auto& r : rows) {
        if (keep >= static_cast<size_t>(kMaxRows)) break;
        float h;
        if (r.collapse) h = kCollapseH;
        else {
            r.n.title = r.n.title.empty() ? r.n.title : r.n.title;
            int lines = 0;
            std::wstring b = r.n.body;
            MeasureBody(b, W - kTextX - kPad, &lines);
            h = std::max(kMinH, kBodyY + kLine * lines + 14);
            if (!lines) h = std::max(kMinH, kTitleY + 18 + 14);
        }
        float use = h + (r.group ? kPeek : 0);
        if (y + use > avail) break;
        r.h = h;
        r.y = y;
        y += use + kGap;
        ++keep;
    }
    rows.resize(keep);
    totalH = keep ? y - kGap : 0;

    std::vector<Card> next;
    int order = 0;
    for (auto& r : rows) {
        std::wstring sig = r.n.title + L"\x1f" + r.n.body + L"\x1f" + Rel(r.n.ft) + L"\x1f" + std::to_wstring(r.count) + L"\x1f" + std::to_wstring(r.h) + (r.group ? L"g" : L"");
        auto it = std::find_if(cards.begin(), cards.end(), [&](Card const& c) { return c.row.key == r.key && !c.gone; });
        if (it != cards.end() && it->row.h == r.h && it->row.group == r.group) {
            Card c = std::move(*it);
            cards.erase(it);
            bool moved = c.row.y != r.y;
            c.row = r;
            if (c.sig != sig) { c.sig = sig; Draw(c, false, false); }
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
            root.Children().InsertAtTop(c.outer);
            AnimateIn(c, firstShow ? order : order);
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

void NotificationStack::Impl::Place() {
    if (!hwnd) return;
    int wPx = static_cast<int>(std::ceil(W * s));
    float h = totalH;
    if (!dying.empty()) h = std::max(h, 1.0f * totalH + 200);  // leave room for fades; trimmed once they are gone
    int hPx = std::max(1, static_cast<int>(std::ceil(h * s)));
    // window shrinks only after fades finish; never let a dying card be clipped by a smaller window
    SetWindowPos(hwnd, HWND_TOPMOST, panel.left, panel.bottom + static_cast<int>(kBelowPanel * s), wPx, hPx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ShowWindow(hwnd, totalH > 0 || !dying.empty() ? SW_SHOWNA : SW_HIDE);
}

void NotificationStack::Impl::Show(RECT p) {
    if (p.right <= p.left) return;
    visible = true;
    if (st == St::Closing) Teardown();
    panel = p;
    if (st == St::Open) { Place(); SetEvent(evReq); return; }
    try {
        HMONITOR hm = MonitorFromRect(&panel, MONITOR_DEFAULTTONEAREST);
        UINT dx = 96, dy = 96;
        GetDpiForMonitor(hm, MDT_EFFECTIVE_DPI, &dx, &dy);
        s = dx / 96.0f;
        W = (panel.right - panel.left) / s;
        BuildFonts();
        hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass, L"Halo Notifications",
                               WS_POPUP, panel.left, panel.bottom, 1, 1, nullptr, nullptr, inst, this);
        if (!hwnd) return;
        BOOL on = TRUE;
        DwmSetWindowAttribute(hwnd, 17 /* DWMWA_USE_HOSTBACKDROPBRUSH */, &on, sizeof(on));
        winrt::check_hresult(comp.as<ICompositorDesktopInterop>()->CreateDesktopWindowTarget(hwnd, TRUE, winrt::put_abi(target)));
        root = comp.CreateContainerVisual();
        root.RelativeSizeAdjustment({1, 1});
        target.Root(root);
        try { noise = NoiseBrush(static_cast<int>(std::ceil(W * s)), 256); } catch (...) { noise = nullptr; }
        st = St::Open;
        if (haveData) Update(true);
        SetEvent(evReq);
    } catch (winrt::hresult_error const& e) {
        svc::Log(L"notifications: show failed: " + std::wstring(e.message().c_str()));
        Teardown();
    }
}

void NotificationStack::Impl::Hide() {
    visible = false;
    if (st != St::Open) return;
    st = St::Closing;
    int i = 0;
    for (auto& c : cards) { (void)i; AnimateOut(c); }
    SetTimer(hwnd, kTimerClose, 150, nullptr);
}

void NotificationStack::Impl::Teardown() {
    if (hwnd) { KillTimer(hwnd, kTimerGone); KillTimer(hwnd, kTimerClose); }
    cards.clear();
    dying.clear();
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

NotificationStack::NotificationStack(HINSTANCE inst, wuc::Compositor comp, wuc::CompositionGraphicsDevice gdev)
    : m(std::make_unique<Impl>(inst, std::move(comp), std::move(gdev))) {
    m->Start();
}
NotificationStack::~NotificationStack() { m->Stop(); }
void NotificationStack::Show(RECT panelScreen) { m->Show(panelScreen); }
void NotificationStack::Hide() { m->Hide(); }

}  // namespace halo
