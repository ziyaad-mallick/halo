#include "notif_feed.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.UI.Notifications.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "icons.h"
#include "services.h"

namespace wun = winrt::Windows::UI::Notifications;

namespace halo::notifs {
namespace {

constexpr UINT kMsgData = WM_APP + 1;
constexpr wchar_t kMsgClass[] = L"HaloNotifFeedMsg";

struct Snapshot {
    std::vector<Notif> items;
    std::map<std::wstring, IconPx> icons;  // appKey -> icon, first sight only
    bool ok = false;
    std::wstring error;
};

std::wstring Clean(std::wstring s) {
    if (s.rfind(L"ms-resource:", 0) == 0) return L"";
    return s;
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

// Box-filters an icon down to at most 64 px so a card draws it without aliasing.
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

// The shell's own AppsFolder item for the AUMID: what Start and the taskbar show.
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

// icons.cpp: the unplated logo from the app's package, else the shell's jumbo icon.
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

bool Dry() { return _wgetenv(L"HALO_TEST") || _wgetenv(L"HALO_NOTIF_DRYRUN"); }

long long NowFt() {
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return (static_cast<long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}

Snapshot DemoSnapshot() {
    Snapshot s;
    s.ok = true;
    const long long now = NowFt(), minute = 600000000LL;
    s.items = {
        {1, L"calendar", L"", L"Calendar", L"Design review", L"Your review starts in ten minutes.", now - minute},
        {2, L"messages", L"", L"Messages", L"Alex", L"The updated designs are ready to review.", now - 4 * minute},
        {3, L"messages", L"", L"Messages", L"Alex", L"Sent the final version.", now - 9 * minute},
        {4, L"files", L"", L"Downloads", L"Download complete", L"Quarterly report.pdf is ready.", now - 40 * minute},
        {5, L"mail", L"", L"Mail", L"Sam Rivera", L"Re: launch notes. Looks good to me, ship it.", now - 130 * minute}};
    return s;
}

}  // namespace

struct Feed::Impl {
    HINSTANCE inst{};
    std::function<void()> changed;
    HWND msgWnd{};
    std::thread worker;
    HANDLE evStop{}, evReq{};
    std::atomic<bool> active{false};

    // UI thread only.
    std::vector<Notif> all;  // newest first
    std::map<std::wstring, IconPx> icons;
    std::set<UINT> hidden;   // dismissed here and not (yet) gone from Windows
    bool ready = false;

    Snapshot Query(std::set<std::wstring>& iconDone);
    void Worker();
    void OnSnapshot(Snapshot& snap);
    static LRESULT CALLBACK MsgProc(HWND, UINT, WPARAM, LPARAM);
};

Snapshot Feed::Impl::Query(std::set<std::wstring>& iconDone) {
    if (_wgetenv(L"HALO_NOTIF_DEMO")) return DemoSnapshot();
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
            // A toast whose app was uninstalled can have no AppInfo; one such toast must not blank the whole list.
            winrt::Windows::ApplicationModel::AppInfo info{nullptr};
            try {
                info = un.AppInfo();
                n.aumid = info.AppUserModelId().c_str();
                n.app = info.DisplayInfo().DisplayName().c_str();
            } catch (...) { info = nullptr; }
            n.appKey = n.aumid.empty() ? n.app : n.aumid;
            try {
                FILETIME f = winrt::clock::to_file_time(un.CreationTime());
                n.ft = (static_cast<long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
            } catch (...) {}
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

            // Icon sources, in order: (1) AppInfo logo, (2) the shell's AppsFolder item, (3) the package's own
            // logo; only then does a card draw a letter tile. Read once per app key.
            IconPx ic;
            const wchar_t* src = nullptr;
            try {
                auto ref = info ? info.DisplayInfo().GetLogo({72, 72}) : nullptr;  // null for some apps (Windows Security)
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
            svc::Log(L"notif feed: icon key=" + cur.appKey + L" source=" + (src ? src : L"none (letter tile)"));
            if (src) snap.icons[cur.appKey] = std::move(ic);
        }
        snap.ok = true;
    } catch (winrt::hresult_error const& e) {
        snap.error = L"0x" + std::to_wstring(static_cast<unsigned>(e.code().value)) + L" " + e.message().c_str();
    }
    return snap;
}

// Sleeps on the folder watch; a change while inactive is re-armed and ignored.
void Feed::Impl::Worker() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    wchar_t local[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    std::wstring dir = std::wstring(local) + L"\\Microsoft\\Windows\\Notifications";
    HANDLE watch = FindFirstChangeNotificationW(dir.c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (watch == INVALID_HANDLE_VALUE) { svc::Log(L"notif feed: cannot watch " + dir); watch = nullptr; }
    std::set<std::wstring> iconDone;
    HANDLE h[3] = {evStop, evReq, watch};
    for (;;) {
        DWORD r = WaitForMultipleObjects(watch ? 3 : 2, h, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;  // a failed wait (a dead handle) would otherwise spin this loop
        if (r == WAIT_OBJECT_0 + 2) {
            FindNextChangeNotification(watch);
            if (!active) continue;  // SetActive(true) asks for a fresh read anyway
            // The database writes in bursts: settle first (and let a quit through: Stop() joins this thread).
            const HANDLE burst[2] = {watch, evStop};
            for (int i = 0; i < 10 && WaitForMultipleObjects(2, burst, FALSE, 250) == WAIT_OBJECT_0; ++i) FindNextChangeNotification(watch);
            if (WaitForSingleObject(evStop, 0) == WAIT_OBJECT_0) break;
        }
        if (!active) continue;
        auto* snap = new Snapshot(Query(iconDone));
        if (!msgWnd || !PostMessageW(msgWnd, kMsgData, 0, reinterpret_cast<LPARAM>(snap))) delete snap;
    }
    if (watch) FindCloseChangeNotification(watch);
}

void Feed::Impl::OnSnapshot(Snapshot& snap) {
    if (!snap.ok) {  // access denied or the listener failed: the panel shows its empty state
        svc::Log(L"notif feed: read failed: " + snap.error);
        if (!ready) { ready = true; if (changed) changed(); }
        return;
    }
    for (auto& [k, ic] : snap.icons) icons[k] = std::move(ic);
    std::sort(snap.items.begin(), snap.items.end(), [](Notif const& a, Notif const& b) { return a.ft > b.ft; });
    all = std::move(snap.items);
    std::set<UINT> present;
    for (auto& n : all) present.insert(n.id);
    for (auto it = hidden.begin(); it != hidden.end();) it = present.count(*it) ? std::next(it) : hidden.erase(it);
    ready = true;
    svc::Log(L"notif feed: " + std::to_wstring(all.size()) + L" in Action Center");
    if (changed) changed();
}

LRESULT CALLBACK Feed::Impl::MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    if (m == kMsgData) {
        std::unique_ptr<Snapshot> snap(reinterpret_cast<Snapshot*>(l));
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        try {
            if (self) self->OnSnapshot(*snap);
        } catch (winrt::hresult_error const& e) {
            svc::Log(L"notif feed: update failed: " + std::wstring(e.message().c_str()));
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

Feed::Feed() : m(std::make_unique<Impl>()) {}
Feed::~Feed() { Stop(); }

void Feed::Start(HINSTANCE inst, std::function<void()> changed) {
    if (m->worker.joinable()) return;
    m->inst = inst;
    m->changed = std::move(changed);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = inst;
    wc.lpfnWndProc = Impl::MsgProc;
    wc.lpszClassName = kMsgClass;
    RegisterClassExW(&wc);
    m->msgWnd = CreateWindowExW(0, kMsgClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, m.get());
    m->evStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m->evReq = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m->worker = std::thread([impl = m.get()] { impl->Worker(); });
}

void Feed::Stop() {
    if (!m->worker.joinable()) return;
    m->active = false;
    m->changed = nullptr;
    SetEvent(m->evStop);
    m->worker.join();
    HWND mw = std::exchange(m->msgWnd, nullptr);
    if (mw) DestroyWindow(mw);
    CloseHandle(m->evStop);
    CloseHandle(m->evReq);
    m->evStop = m->evReq = nullptr;
}

void Feed::SetActive(bool on) {
    if (m->active.exchange(on) == on || !on || !m->evReq) return;
    SetEvent(m->evReq);  // read now
}

bool Feed::Ready() const { return m->ready; }

std::vector<Notif> Feed::Items() const {
    std::vector<Notif> out;
    for (auto const& n : m->all)
        if (!m->hidden.count(n.id)) out.push_back(n);
    return out;
}

IconPx const* Feed::Icon(std::wstring const& appKey) const {
    auto it = m->icons.find(appKey);
    return it == m->icons.end() ? nullptr : &it->second;
}

void Feed::Remove(std::vector<UINT> const& ids) {
    bool any = false;
    for (UINT id : ids) {
        if (Dry()) { m->hidden.insert(id); any = true; svc::Log(L"notif feed: DRY RUN, would remove id " + std::to_wstring(id)); continue; }
        try {
            wun::Management::UserNotificationListener::Current().RemoveNotification(id);
            m->hidden.insert(id);
            any = true;
        } catch (winrt::hresult_error const& e) {
            svc::Log(L"notif feed: RemoveNotification failed, kept: " + std::wstring(e.message().c_str()));
        }
    }
    if (any && m->changed) m->changed();
}

void Feed::Clear() {
    std::vector<UINT> ids;
    for (auto const& n : m->all)
        if (!m->hidden.count(n.id)) ids.push_back(n.id);
    svc::Log(L"notif feed: clear all (" + std::to_wstring(ids.size()) + L")");
    if (ids.empty()) return;
    if (Dry()) { Remove(ids); return; }
    // One call, not a RemoveNotification round trip per toast (each is cross-process: hundreds froze the panel).
    try {
        wun::Management::UserNotificationListener::Current().ClearNotifications();
        for (UINT id : ids) m->hidden.insert(id);
        if (m->changed) m->changed();
    } catch (winrt::hresult_error const& e) {
        svc::Log(L"notif feed: ClearNotifications failed, kept: " + std::wstring(e.message().c_str()));
    }
}

bool Feed::Open(Notif const& n) {
    if (Dry()) { svc::Log(L"notif feed: DRY RUN, would open " + n.aumid + L" and remove id " + std::to_wstring(n.id)); return true; }
    if (n.aumid.empty()) { svc::Log(L"notif feed: no app id, kept"); return false; }
    auto res = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + n.aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    svc::Log(L"notif feed: open " + n.aumid + L" -> " + std::to_wstring(res));
    if (res <= 32) return false;
    Remove({n.id});  // Windows drops a toast once it has been clicked
    return true;
}

}  // namespace halo::notifs
