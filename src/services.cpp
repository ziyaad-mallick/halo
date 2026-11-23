#include "services.h"
#include "shell_launch.h"

#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>
#include <wlanapi.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace halo::svc {
namespace {

// Marks our own volume writes so the change callback can skip echoing them back.
constexpr GUID kOurContext = {0x48414c4f, 0x4343, 0x4343, {0x76, 0x6f, 0x6c, 0x00, 0x00, 0x00, 0x00, 0x01}};

// Undocumented, but the one way to change the default output (what the Sound control panel uses).
struct IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY*, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY*, const PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR id, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};
constexpr IID kIidPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};
constexpr CLSID kClsidPolicyConfig = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

struct VolumeCallback : winrt::implements<VolumeCallback, IAudioEndpointVolumeCallback> {
    VolumeCallback(HWND h, UINT m) : hwnd(h), msg(m) {}
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA d) override {
        if (d && d->guidEventContext != kOurContext) PostMessageW(hwnd, msg, 0, 0);
        return S_OK;
    }
    HWND hwnd;
    UINT msg;
};

// Posts `msg` (wParam kDeviceChanged) when the default endpoint of one direction changes.
struct DeviceCallback : winrt::implements<DeviceCallback, IMMNotificationClient> {
    DeviceCallback(EDataFlow f, HWND h, UINT m) : flow(f), hwnd(h), msg(m) {}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow f, ERole role, LPCWSTR) override {
        if (f == flow && role == eMultimedia) PostMessageW(hwnd, msg, AudioEndpoint::kDeviceChanged, 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    EDataFlow flow;
    HWND hwnd;
    UINT msg;
};

}  // namespace

bool DryRun() {
    static const bool on = [] {
        wchar_t v[4] = {};
        return GetEnvironmentVariableW(L"HALO_TEST_CC_DRYRUN", v, 4) && v[0] == L'1';
    }();
    return on;
}

void Log(std::wstring const& msg) {
    auto line = L"[halo/cc] " + msg + L"\n";
    OutputDebugStringW(line.c_str());
    fputws(line.c_str(), stderr);
    fflush(stderr);
}

namespace {
// Turns a burst of "something changed" pokes into at most one PostMessage per kMinGapMs: the first poke posts at
// once, the rest of a burst is folded into one trailing post. Instances are leaked on purpose (a threadpool timer
// may fire during process exit).
class Coalescer {
public:
    static constexpr ULONGLONG kMinGapMs = 100;
    void SetTarget(HWND h, UINT m) {
        std::lock_guard lock(m_mutex);
        m_hwnd = h;
        m_msg = m;
    }
    void Poke() {
        std::lock_guard lock(m_mutex);
        if (!m_hwnd || m_armed) return;
        const ULONGLONG now = GetTickCount64();
        if (now - m_last >= kMinGapMs) {
            m_last = now;
            PostMessageW(m_hwnd, m_msg, 0, 0);
            return;
        }
        if (!m_timer) m_timer = CreateThreadpoolTimer(&Coalescer::OnTimer, this, nullptr);
        if (!m_timer) { PostMessageW(m_hwnd, m_msg, 0, 0); return; }
        ULARGE_INTEGER due;
        due.QuadPart = static_cast<ULONGLONG>(-static_cast<LONGLONG>((kMinGapMs - (now - m_last)) * 10000));
        FILETIME ft{due.LowPart, due.HighPart};
        m_armed = true;
        SetThreadpoolTimer(m_timer, &ft, 0, 0);
    }
    // For a user-initiated transition (a refresh began, a connect went pending): no folding.
    void PostNow() {
        std::lock_guard lock(m_mutex);
        if (!m_hwnd) return;
        m_last = GetTickCount64();
        PostMessageW(m_hwnd, m_msg, 0, 0);
    }
private:
    static VOID CALLBACK OnTimer(PTP_CALLBACK_INSTANCE, PVOID ctx, PTP_TIMER) {
        auto* self = static_cast<Coalescer*>(ctx);
        std::lock_guard lock(self->m_mutex);
        self->m_armed = false;
        if (!self->m_hwnd) return;
        self->m_last = GetTickCount64();
        PostMessageW(self->m_hwnd, self->m_msg, 0, 0);
    }
    std::mutex m_mutex;
    HWND m_hwnd{};
    UINT m_msg{};
    bool m_armed = false;
    ULONGLONG m_last = 0;
    PTP_TIMER m_timer = nullptr;
};

// Runs `fn` on a detached worker with COM in the MTA.
template <typename F>
bool RunOffThread(F fn) {
    try {
        std::thread([fn = std::move(fn)]() mutable {
            const bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
            try { fn(); } catch (...) {}
            if (co) CoUninitialize();
        }).detach();
        return true;
    } catch (...) {
        return false;
    }
}
}  // namespace

// ---- Audio ----------------------------------------------------------------------------
bool AudioEndpoint::Bind(EDataFlow flow, HWND notify, UINT msg) {
    Unbind();
    winrt::com_ptr<IMMDeviceEnumerator> en;
    winrt::com_ptr<IMMDevice> dev;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
                                en.put_void())) ||
        FAILED(en->GetDefaultAudioEndpoint(flow, eMultimedia, dev.put())) ||
        FAILED(dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr, m_vol.put_void()))) {
        m_vol = nullptr;
        return false;
    }
    m_cb = winrt::make<VolumeCallback>(notify, msg);
    m_vol->RegisterControlChangeNotify(m_cb.get());
    m_flow = flow;
    m_notify = notify;
    m_msg = msg;
    m_en = en;
    m_nc = winrt::make<DeviceCallback>(flow, notify, msg);
    m_en->RegisterEndpointNotificationCallback(m_nc.get());
    return true;
}

void AudioEndpoint::OnMessage(WPARAM w) {
    if (w == kDeviceChanged && m_notify) Bind(m_flow, m_notify, m_msg);  // follow the new default device
}

void AudioEndpoint::Unbind() {
    if (m_en && m_nc) m_en->UnregisterEndpointNotificationCallback(m_nc.get());
    m_nc = nullptr;
    m_en = nullptr;
    if (m_vol && m_cb) m_vol->UnregisterControlChangeNotify(m_cb.get());
    m_cb = nullptr;
    m_vol = nullptr;
}

float AudioEndpoint::Volume() const {
    float v = 0;
    if (m_vol) m_vol->GetMasterVolumeLevelScalar(&v);
    return v;
}

bool AudioEndpoint::Muted() const {
    BOOL m = FALSE;
    if (m_vol) m_vol->GetMute(&m);
    return m != FALSE;
}

void AudioEndpoint::SetVolume(float scalar) {
    scalar = std::clamp(scalar, 0.0f, 1.0f);
    if (DryRun()) return Log(L"DRYRUN volume -> " + std::to_wstring(static_cast<int>(scalar * 100)));
    if (m_vol) m_vol->SetMasterVolumeLevelScalar(scalar, &kOurContext);
}

void AudioEndpoint::SetMute(bool mute) {
    if (DryRun()) return Log(std::wstring(L"DRYRUN mute -> ") + (mute ? L"on" : L"off"));
    if (m_vol) m_vol->SetMute(mute, &kOurContext);
}

std::vector<Device> OutputDevices() {
    std::vector<Device> out;
    winrt::com_ptr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator),
                                en.put_void())))
        return out;
    std::wstring defId;
    winrt::com_ptr<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, def.put()))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id)) && id) { defId = id; CoTaskMemFree(id); }
    }
    winrt::com_ptr<IMMDeviceCollection> col;
    UINT n = 0;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.put())) || FAILED(col->GetCount(&n))) return out;
    for (UINT i = 0; i < n; i++) {
        winrt::com_ptr<IMMDevice> dev;
        LPWSTR id = nullptr;
        if (FAILED(col->Item(i, dev.put())) || FAILED(dev->GetId(&id)) || !id) continue;
        Device d{id, id, false};
        CoTaskMemFree(id);
        d.isDefault = d.id == defId;
        winrt::com_ptr<IPropertyStore> props;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put())) &&
            SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal)
            d.name = pv.pwszVal;
        PropVariantClear(&pv);
        out.push_back(std::move(d));
    }
    return out;
}

// ---- Output devices: cache, change notifications, async default switch ------------------------------------
namespace {
// The blocking part: IPolicyConfig for all three roles, else the Sound settings page. Worker threads only.
AudioOpResult SetDefaultNow(std::wstring const& id) {
    if (DryRun()) { Log(L"DRYRUN default output -> " + id); return AudioOpResult::Succeeded; }
    winrt::com_ptr<IPolicyConfig> pc;
    if (SUCCEEDED(CoCreateInstance(kClsidPolicyConfig, nullptr, CLSCTX_ALL, kIidPolicyConfig, pc.put_void()))) {
        bool ok = true;
        for (ERole r : {eConsole, eMultimedia, eCommunications}) ok &= SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), r));
        if (ok) return AudioOpResult::Succeeded;
    }
    HINSTANCE h = ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(h) > 32 ? AudioOpResult::FellBackToSettings : AudioOpResult::Failed;
}

// Every render endpoint in any state, with its container id (the key that ties a Bluetooth device to its endpoints).
struct EpInfo {
    std::wstring id, name, container;  // container: lower-case, no braces (see NormId)
    DWORD state = 0;
    bool isDefault = false;
    bool operator==(EpInfo const&) const = default;
};

std::wstring NormId(std::wstring const& s) {
    std::wstring out;
    for (wchar_t c : s) if (c != L'{' && c != L'}') out.push_back(static_cast<wchar_t>(towlower(c)));
    return out;
}

// PKEY_Device_ContainerId {8C7ED206-3F8A-4827-B3AB-AE9E1FAEFC6C}, 2: groups every endpoint and node of one physical device.
constexpr PROPERTYKEY kPkeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

std::vector<EpInfo> AllRenderEndpoints() {
    std::vector<EpInfo> out;
    winrt::com_ptr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), en.put_void())))
        return out;
    std::wstring defId;
    winrt::com_ptr<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, def.put()))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id)) && id) { defId = id; CoTaskMemFree(id); }
    }
    winrt::com_ptr<IMMDeviceCollection> col;
    UINT n = 0;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, col.put())) || FAILED(col->GetCount(&n))) return out;
    for (UINT i = 0; i < n; i++) {
        winrt::com_ptr<IMMDevice> dev;
        LPWSTR id = nullptr;
        if (FAILED(col->Item(i, dev.put())) || FAILED(dev->GetId(&id)) || !id) continue;
        EpInfo ep;
        ep.id = id;
        CoTaskMemFree(id);
        ep.isDefault = ep.id == defId;
        dev->GetState(&ep.state);
        winrt::com_ptr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) ep.name = pv.pwszVal;
            PropVariantClear(&pv);
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(kPkeyContainerId, &pv)) && pv.vt == VT_CLSID && pv.puuid) {
                wchar_t g[48] = {};
                StringFromGUID2(*pv.puuid, g, 48);
                ep.container = NormId(g);
            }
            PropVariantClear(&pv);
        }
        out.push_back(std::move(ep));
    }
    return out;
}

// The stereo (A2DP) endpoint is the one to make default; the hands-free (telephony) one is a worse choice. Names are
// all there is to tell them apart, so this is a heuristic: UNVERIFIED on the owner's earphones.
bool LooksHandsFree(std::wstring const& name) {
    std::wstring n = NormId(name);
    return n.find(L"hands-free") != std::wstring::npos || n.find(L"hands free") != std::wstring::npos ||
           n.find(L"handsfree") != std::wstring::npos || n.find(L"freisprech") != std::wstring::npos;
}
EpInfo const* BestEndpoint(std::vector<EpInfo const*> const& eps, bool activeOnly) {
    EpInfo const* best = nullptr;
    for (auto const* ep : eps) {
        if (activeOnly && ep->state != DEVICE_STATE_ACTIVE) continue;
        if (!best || (LooksHandsFree(best->name) && !LooksHandsFree(ep->name))) best = ep;
    }
    return best;
}

struct OutHub {
    std::mutex m;
    std::vector<Device> list;
    std::vector<EpInfo> all;  // every render endpoint in any state
    ULONGLONG updated = 0;
    bool busy = false, dirty = false, subscribed = false;
    std::atomic<int> listeners{0};  // bit 0: the output page, bit 1: the Bluetooth page. Endpoint changes re-enumerate only while one is set
    Coalescer post;
    winrt::com_ptr<IMMDeviceEnumerator> en;
    winrt::com_ptr<IMMNotificationClient> nc;
};
OutHub& Out() {
    static OutHub* h = new OutHub;
    return *h;
}

void OutputsKick();
void OutputsNote();
void BtNoteAudioChanged();  // defined with the Bluetooth hub: the audio facts of a row changed

// Endpoint changes only mark the cache dirty; the callback itself never calls back into the enumerator.
struct OutChanged : winrt::implements<OutChanged, IMMNotificationClient> {
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { OutputsNote(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { OutputsNote(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { OutputsNote(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow f, ERole, LPCWSTR) override {
        if (f == eRender) OutputsNote();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};

bool SameOutputs(std::vector<Device> const& a, std::vector<Device> const& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].isDefault != b[i].isDefault) return false;
    return true;
}

void OutputsWorker() {
    auto& h = Out();
    {   // subscribe once, on first use, from a worker: the notification client stays for the life of the process
        std::lock_guard lock(h.m);
        if (!h.subscribed) {
            h.subscribed = true;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                           __uuidof(IMMDeviceEnumerator), h.en.put_void()))) {
                h.nc = winrt::make<OutChanged>();
                if (FAILED(h.en->RegisterEndpointNotificationCallback(h.nc.get()))) h.nc = nullptr;
            }
        }
    }
    for (;;) {
        { std::lock_guard lock(h.m); h.dirty = false; }
        auto devices = OutputDevices();
        auto all = AllRenderEndpoints();
        bool changed, allChanged;
        {
            std::lock_guard lock(h.m);
            changed = !h.updated || !SameOutputs(h.list, devices);
            allChanged = h.all != all;
            h.list = std::move(devices);
            h.all = std::move(all);
            h.updated = GetTickCount64();
            if (!h.dirty) { h.busy = false; break; }
        }
        if (changed) h.post.Poke();
        if (allChanged) BtNoteAudioChanged();
    }
    BtNoteAudioChanged();
    h.post.PostNow();  // the last enumeration also clears the refreshing flag, so always tell the page
}

void OutputsKick() {
    auto& h = Out();
    {
        std::lock_guard lock(h.m);
        if (h.busy) { h.dirty = true; return; }
        h.busy = true;
    }
    if (!RunOffThread(OutputsWorker)) {
        std::lock_guard lock(h.m);
        h.busy = false;
    }
}
void OutputsNote() {
    if (Out().listeners.load()) OutputsKick();
}
}  // namespace

std::vector<Device> OutputDevicesCached() {
    auto& h = Out();
    std::lock_guard lock(h.m);
    return h.list;
}
void OutputsRefresh(HWND notify, UINT msg) {
    auto& h = Out();
    h.listeners |= 1;
    h.post.SetTarget(notify, msg);
    h.post.PostNow();  // the page re-reads the cache straight away
    OutputsKick();
}
void OutputsStop() {
    Out().listeners &= ~1;
    Out().post.SetTarget(nullptr, 0);
}
ListInfo OutputsInfo() {
    auto& h = Out();
    std::lock_guard lock(h.m);
    return {h.updated != 0, h.busy, h.updated};
}

void SetDefaultAudioOutput(std::wstring const& endpointId, HWND notify, UINT msg, std::function<void(AudioOpResult)> done) {
    auto job = [id = endpointId, notify, msg, done] {
        const AudioOpResult r = SetDefaultNow(id);
        if (done) done(r);
        OutputsKick();
        if (notify) PostMessageW(notify, msg, 0, 0);
    };
    if (!RunOffThread(job)) {
        if (done) done(AudioOpResult::Failed);
    }
}

bool SetDefaultOutput(std::wstring const& id) {
    SetDefaultAudioOutput(id, nullptr, 0);
    return true;
}

// ---- Power ----------------------------------------------------------------------------
Battery ReadBattery() {
    Battery b;
    SYSTEM_POWER_STATUS s{};
    if (GetSystemPowerStatus(&s)) {
        b.hasBattery = !(s.BatteryFlag & 128) && s.BatteryLifePercent <= 100;
        b.percent = s.BatteryLifePercent <= 100 ? s.BatteryLifePercent : 100;
        b.remainingSeconds = s.BatteryLifeTime == DWORD(-1) ? -1 : static_cast<int>(s.BatteryLifeTime);
        b.ac = s.ACLineStatus == 1;
        b.charging = (s.BatteryFlag & 8) != 0;
    }
    return b;
}

namespace {
// Windows 11 power modes are "overlay schemes" over the active plan (Settings > System > Power).
constexpr GUID kOverlay[] = {
    {0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}},  // best power efficiency
    {0, 0, 0, {0, 0, 0, 0, 0, 0, 0, 0}},                                              // balanced
    {0x3af9b8d9, 0x7c97, 0x431d, {0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f}},  // best performance
    {0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}},  // max performance
};
}  // namespace

PowerMode ReadPowerMode() {
    using Fn = DWORD(WINAPI*)(GUID*);
    static auto fn = reinterpret_cast<Fn>(GetProcAddress(LoadLibraryW(L"powrprof.dll"), "PowerGetEffectiveOverlayScheme"));
    GUID g{};
    if (!fn || fn(&g) != ERROR_SUCCESS) return PowerMode::Unknown;
    for (int i = 0; i < 4; i++)
        if (IsEqualGUID(g, kOverlay[i])) return static_cast<PowerMode>(i);
    return PowerMode::Unknown;
}

PowerMode NextPowerMode(PowerMode m) {
    switch (m) {
        case PowerMode::Efficiency: return PowerMode::Balanced;
        case PowerMode::Balanced: return PowerMode::Performance;
        default: return PowerMode::Efficiency;
    }
}

void SetPowerMode(PowerMode m) {
    if (m == PowerMode::Unknown) return;
    if (DryRun()) return Log(std::wstring(L"DRYRUN power mode -> ") + PowerModeName(m));
    using Fn = DWORD(WINAPI*)(GUID);
    static auto fn = reinterpret_cast<Fn>(GetProcAddress(LoadLibraryW(L"powrprof.dll"), "PowerSetActiveOverlayScheme"));
    if (fn) fn(kOverlay[static_cast<int>(m)]);
}

wchar_t const* PowerModeName(PowerMode m) {
    switch (m) {
        case PowerMode::Efficiency: return L"Best power efficiency";
        case PowerMode::Balanced: return L"Balanced";
        case PowerMode::Performance: return L"Best performance";
        case PowerMode::Max: return L"Max performance";
        default: return L"Power mode";
    }
}

// ---- Do Not Disturb ---------------------------------------------------------------------
namespace {
constexpr wchar_t kNotifKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings";
constexpr wchar_t kToastsValue[] = L"NOC_GLOBAL_SETTING_TOASTS_ENABLED";
}  // namespace

bool ReadDnd() {
    DWORD v = 1, n = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kNotifKey, kToastsValue, RRF_RT_REG_DWORD, nullptr, &v, &n);
    return v == 0;  // an absent value means toasts are on
}

void SetDnd(bool on) {
    if (DryRun()) return Log(std::wstring(L"DRYRUN do not disturb -> ") + (on ? L"on" : L"off"));
    DWORD v = on ? 0 : 1;
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kNotifKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(k, kToastsValue, 0, REG_DWORD, reinterpret_cast<BYTE const*>(&v), sizeof(v));
        RegCloseKey(k);
    }
}

// ---- Brightness -----------------------------------------------------------------------------
namespace brightness {
namespace {

struct State {
    std::atomic<int> level{-1};
    std::atomic<bool> supported{false};
    std::mutex wmi;  // one WMI conversation at a time, including probes
    std::mutex queue;
    int wanted = -1, applied = -2;
    unsigned request = 0;
    bool running = false;
};

std::shared_ptr<State> Shared() {
    static auto state = std::make_shared<State>();
    return state;
}

winrt::com_ptr<IWbemServices> Connect() {
    winrt::com_ptr<IWbemLocator> loc;
    winrt::com_ptr<IWbemServices> svc;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, loc.put_void())))
        return nullptr;
    BSTR ns = SysAllocString(L"ROOT\\WMI");
    HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, svc.put());
    SysFreeString(ns);
    if (FAILED(hr)) return nullptr;
    CoSetProxyBlanket(svc.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return svc;
}

// First active instance of `cls`, or null.
winrt::com_ptr<IWbemClassObject> First(IWbemServices* svc, wchar_t const* cls) {
    winrt::com_ptr<IEnumWbemClassObject> en;
    std::wstring q = std::wstring(L"SELECT * FROM ") + cls + L" WHERE Active=TRUE";
    BSTR lang = SysAllocString(L"WQL"), query = SysAllocString(q.c_str());
    HRESULT hr = svc->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, en.put());
    SysFreeString(lang);
    SysFreeString(query);
    winrt::com_ptr<IWbemClassObject> obj;
    ULONG n = 0;
    if (SUCCEEDED(hr) && en) en->Next(5000, 1, obj.put(), &n);
    return n ? obj : nullptr;
}

void Read(State& state) {
    if (auto svc = Connect())
        if (auto obj = First(svc.get(), L"WmiMonitorBrightness")) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr)) && v.vt != VT_NULL) {
                state.level = v.bVal;
                state.applied = v.bVal;
                state.supported = true;
            }
            VariantClear(&v);
        }
}

bool Write(int pct) {
    auto svc = Connect();
    if (!svc) return false;
    auto inst = First(svc.get(), L"WmiMonitorBrightnessMethods");
    if (!inst) return false;
    VARIANT path;
    VariantInit(&path);
    BSTR cls = SysAllocString(L"WmiMonitorBrightnessMethods");
    winrt::com_ptr<IWbemClassObject> clsObj, inDef, in;
    HRESULT hr = svc->GetObject(cls, 0, nullptr, clsObj.put(), nullptr);
    SysFreeString(cls);
    bool ok = false;
    if (SUCCEEDED(hr) && SUCCEEDED(inst->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR &&
        SUCCEEDED(clsObj->GetMethod(L"WmiSetBrightness", 0, inDef.put(), nullptr)) &&
        SUCCEEDED(inDef->SpawnInstance(0, in.put()))) {
        VARIANT t, b;
        VariantInit(&t);
        VariantInit(&b);
        t.vt = VT_I4; t.lVal = 1;
        b.vt = VT_I4; b.lVal = pct;
        const HRESULT timeoutHr = in->Put(L"Timeout", 0, &t, 0);
        const HRESULT brightnessHr = in->Put(L"Brightness", 0, &b, 0);
        BSTR method = SysAllocString(L"WmiSetBrightness");
        winrt::com_ptr<IWbemClassObject> result;
        if (SUCCEEDED(timeoutHr) && SUCCEEDED(brightnessHr) && method &&
            SUCCEEDED(svc->ExecMethod(path.bstrVal, method, 0, nullptr, in.get(), result.put(), nullptr)) && result) {
            VARIANT value;
            VariantInit(&value);
            if (SUCCEEDED(result->Get(L"ReturnValue", 0, &value, nullptr, nullptr)))
                ok = (value.vt == VT_I4 && value.lVal == 0) || (value.vt == VT_UI4 && value.ulVal == 0);
            VariantClear(&value);
        }
        SysFreeString(method);
    }
    VariantClear(&path);
    return ok;
}

}  // namespace

void Probe(HWND notify, UINT msg) {
    std::thread([notify, msg, state = Shared()] {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            { std::lock_guard<std::mutex> lock(state->wmi); Read(*state); }
            CoUninitialize();
        }
        PostMessageW(notify, msg, 0, 0);
    }).detach();
}

bool Supported() { return Shared()->supported; }
int Level() { return Shared()->level; }

void Set(int percent) {
    percent = std::clamp(percent, 0, 100);
    auto state = Shared();
    state->level = percent;
    if (DryRun()) return Log(L"DRYRUN brightness -> " + std::to_wstring(percent));
    std::lock_guard lock(state->queue);
    state->wanted = percent;
    ++state->request;
    if (state->running) return;
    state->running = true;
    try {
        std::thread([state] {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
                std::lock_guard lock(state->queue);
                state->running = false;
                return;
            }
            for (;;) {
                int value;
                unsigned request;
                {
                    std::lock_guard lock(state->queue);
                    value = state->wanted;
                    request = state->request;
                }
                {
                    std::lock_guard lock(state->wmi);
                    if (value != state->applied) {
                        if (Write(value)) state->applied = value;
                        else Log(L"brightness: write failed; target remains retryable");
                    }
                }
                std::lock_guard lock(state->queue);
                if (request == state->request) {
                    // Stop before admitting another worker. The shared state stays alive even on exit.
                    CoUninitialize();
                    state->running = false;
                    return;
                }
            }
        }).detach();
    } catch (std::exception const&) {
        state->running = false;
        Log(L"brightness: worker could not start");
    }
}

}  // namespace brightness

// ---- Radios ---------------------------------------------------------------------------------
namespace wdr = winrt::Windows::Devices::Radios;

namespace {
// The radios found last time, kept for the life of the process: a Start() after the first one has them at once
// (the old code re-ran GetRadiosAsync on every Start-panel open, and the tiles ignored clicks until it returned).
std::mutex g_radioCacheMutex;
std::vector<wdr::Radio> g_radioCache;
bool g_radioCacheValid = false;
std::atomic<bool> g_radioAccessGranted{false};

// An unplugged USB adapter leaves a closed Radio whose State() throws.
bool RadioIsDead(wdr::Radio const& r) {
    try { (void)r.State(); return false; } catch (...) { return true; }
}
}  // namespace

void Radios::Start(HWND notify, UINT msg) {
    Stop();
    m_notify = notify;
    m_msg = msg;
    {
        std::lock_guard lock(g_radioCacheMutex);
        if (g_radioCacheValid) Adopt(g_radioCache, notify, msg);
    }
    m_loading = !m_wifi && !m_bt;
    Find(notify, msg, m_gen, *m_gen);  // always re-check in the background: an adapter may have come or gone
    try {
        // The tile's SSID follows the network, not only the radio. The read happens off-thread; the UI gets a message.
        namespace wncs = winrt::Windows::Networking::Connectivity;
        auto token = wncs::NetworkInformation::NetworkStatusChanged(
            [notify, msg](winrt::Windows::Foundation::IInspectable const&) { SsidRefresh(notify, msg); });
        m_netUnsub = [token] {
            try { wncs::NetworkInformation::NetworkStatusChanged(token); } catch (...) {}
        };
    } catch (...) {}
    SsidRefresh(notify, msg);
}

void Radios::Stop() {
    ++*m_gen;
    if (m_netUnsub) { m_netUnsub(); m_netUnsub = nullptr; }
    m_wifiRev.revoke();
    m_btRev.revoke();
    m_wifi = nullptr;
    m_bt = nullptr;
    m_loading = m_failed = false;
}

void Radios::Adopt(std::vector<wdr::Radio> const& radios, HWND notify, UINT msg) {
    auto handler = [notify, msg](auto&&, auto&&) { PostMessageW(notify, msg, 0, 0); };
    for (auto const& r : radios) {
        try {
            if (r.Kind() == wdr::RadioKind::WiFi && (!m_wifi || RadioIsDead(m_wifi))) {
                m_wifiRev.revoke();
                m_wifi = r;
                m_wifiRev = r.StateChanged(winrt::auto_revoke, handler);
            }
            if (r.Kind() == wdr::RadioKind::Bluetooth && (!m_bt || RadioIsDead(m_bt))) {
                m_btRev.revoke();
                m_bt = r;
                m_btRev = r.StateChanged(winrt::auto_revoke, handler);
            }
        } catch (...) {}
    }
}

winrt::fire_and_forget Radios::Find(HWND notify, UINT msg, std::shared_ptr<unsigned> live, unsigned gen) {
    winrt::apartment_context ui;  // the thread that owns this object
    std::vector<wdr::Radio> found;
    bool failed = false;
    try {
        co_await winrt::resume_background();
        auto radios = co_await wdr::Radio::GetRadiosAsync();
        for (auto const& r : radios) found.push_back(r);
    } catch (...) {
        failed = true;
    }
    co_await ui;
    if (gen != *live) co_return;
    if (failed) {
        if (!m_wifi && !m_bt) m_failed = true;
    } else {
        {
            std::lock_guard lock(g_radioCacheMutex);
            g_radioCache = found;
            g_radioCacheValid = true;
        }
        Adopt(found, notify, msg);
    }
    m_loading = false;
    PostMessageW(notify, msg, 0, 0);
}

static bool RadioIsOn(wdr::Radio const& r) {
    try { return r && r.State() == wdr::RadioState::On; } catch (...) { return false; }
}
bool Radios::WifiOn() const {
    const int want = m_wifiOp->want.load();
    return want >= 0 ? want == 1 : RadioIsOn(m_wifi);
}
bool Radios::BluetoothOn() const {
    const int want = m_btOp->want.load();
    return want >= 0 ? want == 1 : RadioIsOn(m_bt);
}

// Everything slow (the access request, SetStateAsync) runs on a pool thread; the caller only records the request.
// Requests that arrive while one is running replace the pending target; the running worker applies the latest.
winrt::fire_and_forget Radios::Set(wdr::Radio radio, std::shared_ptr<Op> op, bool on, HWND notify, UINT msg) {
    if (!radio) co_return;
    if (DryRun()) { Log(std::wstring(L"DRYRUN radio ") + (radio.Kind() == wdr::RadioKind::WiFi ? L"wifi" : L"bluetooth") +
                        (on ? L" -> on" : L" -> off")); co_return; }
    op->want = on ? 1 : 0;
    if (op->busy.exchange(true)) co_return;  // the running worker picks up the new target
    co_await winrt::resume_background();
    for (;;) {
        try {
            for (;;) {
                const int w = op->want.load();
                if (w < 0) break;
                if (!g_radioAccessGranted.load()) {
                    if (co_await wdr::Radio::RequestAccessAsync() == wdr::RadioAccessStatus::Allowed) g_radioAccessGranted = true;
                }
                if (g_radioAccessGranted.load())
                    co_await radio.SetStateAsync(w == 1 ? wdr::RadioState::On : wdr::RadioState::Off);
                int expected = w;
                if (op->want.compare_exchange_strong(expected, -1)) break;
            }
        } catch (...) {
            op->want = -1;
        }
        op->busy = false;
        if (op->want.load() < 0 || op->busy.exchange(true)) break;  // a request slipped in while we were finishing
    }
    PostMessageW(notify, msg, 0, 0);  // the tile was flipped optimistically: if nothing changed, this puts the truth back
}

std::wstring ConnectedSsid() {
    try {
        namespace wnc = winrt::Windows::Networking::Connectivity;
        for (auto const& profile : wnc::NetworkInformation::GetConnectionProfiles()) {
            try {
                if (profile.IsWlanConnectionProfile() &&
                    profile.GetNetworkConnectivityLevel() != wnc::NetworkConnectivityLevel::None) {
                    auto details = profile.WlanConnectionProfileDetails();
                    if (details) {
                        std::wstring ssid = details.GetConnectedSsid().c_str();
                        if (!ssid.empty()) return ssid;
                    }
                }
            } catch (...) {}  // an unavailable adapter must not hide another adapter's connection
        }
    } catch (...) {}
    return {};
}

namespace {
std::mutex g_ssidMutex;
std::wstring g_ssid;
bool g_ssidKnown = false, g_ssidBusy = false, g_ssidDirty = false;
}  // namespace

std::wstring CachedSsid() {
    std::lock_guard lock(g_ssidMutex);
    return g_ssid;
}

// One off-thread ConnectedSsid() read at a time; a request that arrives during a read triggers one more read after it.
void SsidRefresh(HWND notify, UINT msg) {
    {
        std::lock_guard lock(g_ssidMutex);
        if (g_ssidBusy) { g_ssidDirty = true; return; }
        g_ssidBusy = true;
    }
    if (!RunOffThread([notify, msg] {
            bool changed = false;
            for (;;) {
                auto ssid = ConnectedSsid();
                {
                    std::lock_guard lock(g_ssidMutex);
                    changed |= !g_ssidKnown || ssid != g_ssid;
                    g_ssid = std::move(ssid);
                    g_ssidKnown = true;
                    if (!g_ssidDirty) { g_ssidBusy = false; break; }
                    g_ssidDirty = false;
                }
            }
            if (changed && notify) PostMessageW(notify, msg, 0, 0);
        })) {
        std::lock_guard lock(g_ssidMutex);
        g_ssidBusy = false;
    }
}

// ---- Quick toggles ------------------------------------------------------------------------
namespace {
constexpr wchar_t kThemeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
}

bool ReadDarkMode() {
    DWORD v = 1, n = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, kThemeKey, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n);
    return v == 0;
}

void SetDarkMode(bool on) {
    if (DryRun()) return Log(std::wstring(L"DRYRUN dark mode -> ") + (on ? L"on" : L"off"));
    DWORD v = on ? 0 : 1;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kThemeKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    RegSetValueExW(k, L"AppsUseLightTheme", 0, REG_DWORD, reinterpret_cast<BYTE const*>(&v), sizeof(v));
    RegSetValueExW(k, L"SystemUsesLightTheme", 0, REG_DWORD, reinterpret_cast<BYTE const*>(&v), sizeof(v));
    RegCloseKey(k);
    Log(std::wstring(L"dark mode -> ") + (on ? L"on" : L"off") + L" (AppsUseLightTheme=" + std::to_wstring(v) + L")");
    // The broadcast waits for every top-level window to answer (up to 200 ms each): never on the caller's thread.
    try {
        std::thread([] {
            DWORD_PTR r;
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 200, &r);
        }).detach();
    } catch (...) {}
}

// Night light lives in a CloudStore blob (undocumented). Observed on this build by toggling the real
// Settings button and diffing: byte 18 is the payload length, payload starts at 19 ("43 42 01 00").
//   off: len 0x13, payload  43 42 01 00 | D0 0A 02 | C6 14 <ts varint> ...
//   on : len 0x15, payload  43 42 01 00 | 10 00 D0 0A 02 | C6 14 <ts varint> ...
//   (schedule-active form: len 0x12, 10 00 then C6 14 directly, no D0 0A 02)
// So "on" is the 10 00 pair at offset 23; the shell re-serializes the blob (timestamps) on its own.
namespace {
constexpr wchar_t kNightKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
    L"default$windows.data.bluelightreduction.bluelightreductionstate\\windows.data.bluelightreduction.bluelightreductionstate";
bool ReadNightBlob(std::vector<BYTE>& b) {
    DWORD n = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kNightKey, L"Data", RRF_RT_REG_BINARY, nullptr, nullptr, &n) != ERROR_SUCCESS || n < 30) return false;
    b.resize(n);
    if (RegGetValueW(HKEY_CURRENT_USER, kNightKey, L"Data", RRF_RT_REG_BINARY, nullptr, b.data(), &n) != ERROR_SUCCESS) return false;
    b.resize(n);
    return n > 26 && b[19] == 0x43 && b[20] == 0x42;
}
}  // namespace

bool ReadNightLight() {
    std::vector<BYTE> b;
    return ReadNightBlob(b) && b[23] == 0x10 && b[24] == 0x00;
}

void SetNightLight(bool on) {
    if (DryRun()) return Log(std::wstring(L"DRYRUN night light -> ") + (on ? L"on" : L"off"));
    std::vector<BYTE> b;
    if (!ReadNightBlob(b)) return Log(L"night light: blob not found");
    bool isOn = b[23] == 0x10 && b[24] == 0x00;
    if (isOn == on) return;
    if (on) {
        b.insert(b.begin() + 23, {0x10, 0x00});
        b[18] = static_cast<BYTE>(b[18] + 2);
    } else if (b.size() > 27 && b[25] == 0xD0) {  // 10 00 D0 0A 02 -> D0 0A 02
        b.erase(b.begin() + 23, b.begin() + 25);
        b[18] = static_cast<BYTE>(b[18] - 2);
    } else {  // schedule-active form: 10 00 -> D0 0A 02
        b.erase(b.begin() + 23, b.begin() + 25);
        b.insert(b.begin() + 23, {0xD0, 0x0A, 0x02});
        b[18] = static_cast<BYTE>(b[18] + 1);
    }
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kNightKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return Log(L"night light: cannot open key");
    LSTATUS r = RegSetValueExW(k, L"Data", 0, REG_BINARY, b.data(), static_cast<DWORD>(b.size()));
    RegCloseKey(k);
    Log(std::wstring(L"night light -> ") + (on ? L"on" : L"off") + L" (reg " + std::to_wstring(r) + L")");
}

bool ReadEnergySaver() {
    SYSTEM_POWER_STATUS s{};
    return GetSystemPowerStatus(&s) && (s.SystemStatusFlag & 1);
}

void OpenSettings(wchar_t const* uri) {
    if (DryRun()) return Log(std::wstring(L"DRYRUN open ") + uri);
    launch::Open(uri);
}

void SendWinChord(WORD key, bool shift) {
    if (DryRun()) return Log(std::wstring(L"DRYRUN chord Win+") + (shift ? L"Shift+" : L"") + std::to_wstring(key));
    std::vector<INPUT> in;
    auto add = [&](WORD vk, bool up) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        in.push_back(i);
    };
    add(VK_LWIN, false);
    if (shift) add(VK_SHIFT, false);
    add(key, false);
    add(key, true);
    if (shift) add(VK_SHIFT, true);
    add(VK_LWIN, true);
    SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
}

namespace wno = winrt::Windows::Networking::NetworkOperators;
namespace wnc = winrt::Windows::Networking::Connectivity;

bool HotspotAvailable() {
    try {
        auto p = wnc::NetworkInformation::GetInternetConnectionProfile();
        return p && wno::NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(p) == wno::TetheringCapability::Enabled;
    } catch (...) { return false; }
}

bool ReadHotspot() {
    try {
        auto p = wnc::NetworkInformation::GetInternetConnectionProfile();
        return p && wno::NetworkOperatorTetheringManager::CreateFromConnectionProfile(p).TetheringOperationalState() == wno::TetheringOperationalState::On;
    } catch (...) { return false; }
}

static winrt::fire_and_forget HotspotSet(bool on) {
    try {
        co_await winrt::resume_background();  // the tethering manager lookup is a synchronous RPC: not on the caller's thread
        auto mgr = wno::NetworkOperatorTetheringManager::CreateFromConnectionProfile(wnc::NetworkInformation::GetInternetConnectionProfile());
        if (on) co_await mgr.StartTetheringAsync(); else co_await mgr.StopTetheringAsync();
    } catch (...) {}
}

void SetHotspot(bool on) {
    if (DryRun()) return Log(std::wstring(L"DRYRUN hotspot -> ") + (on ? L"on" : L"off"));
    HotspotSet(on);
}

// ---- Wi-Fi discovery and paired Bluetooth devices -----------------------------------------------
namespace {
using radio_policy::DiscoveryPhase;

std::mutex g_wifiMutex, g_wifiScanMutex;
std::vector<WifiNet> g_wifiNets;
DiscoveryStatus g_wifiStatus;
std::atomic<unsigned> g_wifiGen{0};
HWND g_wifiNotify{};
UINT g_wifiMsg{};

struct ScanSignal {
    std::mutex mutex;
    std::condition_variable changed;
    GUID interfaceId{};
    bool complete = false, cancelled = false;
    DWORD result = ERROR_SUCCESS;
};
std::weak_ptr<ScanSignal> g_activeWifiScan;
std::mutex g_scanRegistryMutex;
std::unordered_map<void*, std::weak_ptr<ScanSignal>> g_scanRegistry;

VOID WINAPI WlanNotify(PWLAN_NOTIFICATION_DATA data, PVOID context) {
    if (!data || data->NotificationSource != WLAN_NOTIFICATION_SOURCE_ACM ||
        (data->NotificationCode != wlan_notification_acm_scan_complete &&
         data->NotificationCode != wlan_notification_acm_scan_fail)) return;
    std::shared_ptr<ScanSignal> signal;
    {
        std::lock_guard lock(g_scanRegistryMutex);
        auto it = g_scanRegistry.find(context);
        if (it != g_scanRegistry.end()) signal = it->second.lock();
    }
    if (!signal) return;
    std::lock_guard lock(signal->mutex);
    if (!IsEqualGUID(signal->interfaceId, data->InterfaceGuid)) return;
    signal->complete = true;
    signal->result = data->NotificationCode == wlan_notification_acm_scan_complete ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
    signal->changed.notify_all();
}

struct WlanNotificationRegistration {
    HANDLE handle{};
    std::shared_ptr<ScanSignal> signal;
    bool active = false;
    void Reset() {
        if (!active) return;
        WlanRegisterNotification(handle, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
        {
            std::lock_guard lock(g_scanRegistryMutex);
            g_scanRegistry.erase(signal.get());
        }
        active = false;
    }
    ~WlanNotificationRegistration() { Reset(); }
};

struct WlanHandle {
    HANDLE value{};
    ~WlanHandle() { if (value) WlanCloseHandle(value, nullptr); }
    DWORD Open() {
        DWORD version = 0;
        return WlanOpenHandle(2, nullptr, &version, &value);
    }
};

struct WlanMemory {
    void* value{};
    ~WlanMemory() { if (value) WlanFreeMemory(value); }
    template<typename T> T* as() const { return static_cast<T*>(value); }
};

bool WlanProfileConnected(GUID const& interfaceId, std::wstring const& profileName, DWORD& error) {
    WlanHandle wlan;
    error = wlan.Open();
    if (error != ERROR_SUCCESS) return false;
    WlanMemory connection;
    DWORD bytes = 0;
    error = WlanQueryInterface(wlan.value, &interfaceId, wlan_intf_opcode_current_connection, nullptr,
                               &bytes, &connection.value, nullptr);
    if (error != ERROR_SUCCESS) return false;
    auto attributes = connection.as<WLAN_CONNECTION_ATTRIBUTES>();
    return attributes && attributes->isState == wlan_interface_state_connected &&
           profileName == attributes->strProfileName;
}

void ClearWifiPendingLocked() {
    for (auto& net : g_wifiNets) net.pending = false;
}

// Polls the OS until the profile is the connected one: every 300 ms for ~10 s (the old 1 s step added up to a
// second of latency to "Connected").
void WifiConnectionReadback(unsigned gen, HWND notify, UINT msg, std::wstring profileName, GUID interfaceId) {
    try {
        for (int attempt = 0; attempt < 34 && gen == g_wifiGen.load(); ++attempt) {
            Sleep(300);
            if (gen != g_wifiGen.load()) return;
            DWORD error = ERROR_SUCCESS;
            if (!WlanProfileConnected(interfaceId, profileName, error)) {
                if (error == ERROR_ACCESS_DENIED) {
                    {
                        std::lock_guard lock(g_wifiMutex);
                        if (gen == g_wifiGen.load()) { g_wifiStatus = {DiscoveryPhase::AccessDenied, error}; ClearWifiPendingLocked(); }
                    }
                    if (notify) PostMessageW(notify, msg, 0, 0);
                    return;
                }
                continue;
            }
            {
                std::lock_guard lock(g_wifiMutex);
                if (gen != g_wifiGen.load()) return;
                for (auto& net : g_wifiNets)
                    net.connected = net.interfaceId == interfaceId && net.profileName == profileName;
                std::stable_sort(g_wifiNets.begin(), g_wifiNets.end(), [](WifiNet const& a, WifiNet const& b) {
                    return a.connected > b.connected;
                });
                ClearWifiPendingLocked();
                g_wifiStatus = {DiscoveryPhase::Ready, ERROR_SUCCESS};
            }
            SsidRefresh(notify, msg);  // the tile's SSID text
            if (notify) PostMessageW(notify, msg, 0, 0);
            return;
        }
        {
            std::lock_guard lock(g_wifiMutex);
            if (gen != g_wifiGen.load()) return;
            g_wifiStatus = {DiscoveryPhase::Failed, ERROR_TIMEOUT};
            ClearWifiPendingLocked();
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
    } catch (...) {
        std::lock_guard lock(g_wifiMutex);
        if (gen == g_wifiGen.load()) { g_wifiStatus = {DiscoveryPhase::Failed, ERROR_GEN_FAILURE}; ClearWifiPendingLocked(); }
        if (notify) PostMessageW(notify, msg, 0, 0);
    }
}

std::wstring SsidText(DOT11_SSID const& ssid) {
    if (!ssid.uSSIDLength || ssid.uSSIDLength > DOT11_SSID_MAX_LENGTH) return {};
    auto bytes = reinterpret_cast<char const*>(ssid.ucSSID);
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, static_cast<int>(ssid.uSSIDLength), nullptr, 0);
    if (n > 0) {
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, static_cast<int>(ssid.uSSIDLength), out.data(), n);
        return out;
    }
    // SSIDs are octet strings, not required to be UTF-8. Preserve every byte for display/matching.
    std::wstring out;
    out.reserve(ssid.uSSIDLength);
    for (ULONG i = 0; i < ssid.uSSIDLength; ++i) out.push_back(static_cast<wchar_t>(ssid.ucSSID[i]));
    return out;
}

std::atomic<ULONGLONG> g_wifiStamp{0};  // GetTickCount64() of the last applied list; 0 = never

std::wstring WifiKey(WifiNet const& n) {
    wchar_t guid[48] = {};
    StringFromGUID2(n.interfaceId, guid, 48);
    return n.ssid + L'\x1f' + guid;
}

int CompareNoCase(std::wstring const& a, std::wstring const& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) - CSTR_EQUAL;
}

// One row per SSID and adapter (Windows lists an SSID once per security type), a stable key on every row, the
// pending flag of an in-flight connect carried over, and a stable order: connected first, then by name, then key.
// A refresh therefore never reorders rows that did not change.
void NormalizeWifi(std::vector<WifiNet>& nets) {
    std::vector<std::wstring> pendingKeys;
    {
        std::lock_guard lock(g_wifiMutex);
        for (auto const& n : g_wifiNets) if (n.pending) pendingKeys.push_back(n.id);
    }
    std::vector<WifiNet> out;
    std::unordered_map<std::wstring, size_t> index;
    for (auto& n : nets) {
        n.id = WifiKey(n);
        auto it = index.find(n.id);
        if (it == index.end()) {
            index[n.id] = out.size();
            out.push_back(std::move(n));
            continue;
        }
        auto& kept = out[it->second];
        kept.connected = kept.connected || n.connected;
        kept.bars = std::max(kept.bars, n.bars);
        if (kept.profileName.empty()) kept.profileName = n.profileName;
        kept.secure = kept.secure && n.secure;
    }
    for (auto& n : out)
        n.pending = std::find(pendingKeys.begin(), pendingKeys.end(), n.id) != pendingKeys.end();
    std::stable_sort(out.begin(), out.end(), [](WifiNet const& a, WifiNet const& b) {
        if (a.connected != b.connected) return a.connected > b.connected;
        if (int c = CompareNoCase(a.ssid, b.ssid)) return c < 0;
        return a.id < b.id;
    });
    nets = std::move(out);
}

// Appends the OS's current (cached or fresh) list for one adapter.
DWORD ReadWifiList(HANDLE wlan, GUID const& iface, std::vector<WifiNet>& nets) {
    WlanMemory available;
    auto status = WlanGetAvailableNetworkList(wlan, &iface, 0, nullptr,
                                              reinterpret_cast<PWLAN_AVAILABLE_NETWORK_LIST*>(&available.value));
    if (status != ERROR_SUCCESS) return status;
    auto networks = available.as<WLAN_AVAILABLE_NETWORK_LIST>();
    for (DWORD j = 0; networks && j < networks->dwNumberOfItems; ++j) {
        auto const& item = networks->Network[j];
        auto ssid = SsidText(item.dot11Ssid);
        if (ssid.empty()) continue;
        std::wstring profile = item.strProfileName;
        nets.push_back({std::move(ssid), radio_policy::SignalBars(item.wlanSignalQuality),
                        item.bSecurityEnabled != FALSE,
                        (item.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0,
                        std::move(profile), iface});
    }
    return ERROR_SUCCESS;
}

// Rows from the OS's cached scan, published while the real scan is still running (status stays Loading).
void PublishWifiPartial(unsigned gen, HWND notify, UINT msg, std::vector<WifiNet> nets) {
    NormalizeWifi(nets);
    {
        std::lock_guard lock(g_wifiMutex);
        if (gen != g_wifiGen.load()) return;
        g_wifiNets = std::move(nets);
        g_wifiStamp = GetTickCount64();
    }
    if (notify) PostMessageW(notify, msg, 0, 0);
}

void FinishWifi(unsigned gen, HWND notify, UINT msg, std::vector<WifiNet> nets, DWORD error, bool hadAdapter, bool hadSuccess = false) {
    if (gen != g_wifiGen.load()) return;
    std::stable_sort(nets.begin(), nets.end(), [](WifiNet const& a, WifiNet const& b) {
        return a.connected > b.connected;
    });
    {
        std::lock_guard lock(g_wifiMutex);
        if (gen != g_wifiGen.load()) return;
        g_wifiNets = std::move(nets);
        g_wifiStatus = {hadSuccess || error == ERROR_SUCCESS ? DiscoveryPhase::Ready :
                        !hadAdapter && error == ERROR_NOT_FOUND ? DiscoveryPhase::Unavailable :
                        radio_policy::PhaseForWlanError(error), error};
        g_activeWifiScan.reset();
    }
    if (notify) PostMessageW(notify, msg, 0, 0);
}

// FinishWifi plus normalisation and the freshness stamp, for a result that will actually be applied.
void CompleteWifi(unsigned gen, HWND notify, UINT msg, std::vector<WifiNet> nets, DWORD error, bool hadAdapter, bool hadSuccess = false) {
    const bool usable = hadSuccess || error == ERROR_SUCCESS;
    if (usable) NormalizeWifi(nets);
    if (usable && gen == g_wifiGen.load()) g_wifiStamp = GetTickCount64();
    FinishWifi(gen, notify, msg, std::move(nets), error, hadAdapter, hadSuccess);
}

void WifiScanTaskBody(unsigned gen, HWND notify, UINT msg, std::shared_ptr<ScanSignal> signal) {
    std::unique_lock scanLock(g_wifiScanMutex);
    auto cancelled = [&] { return gen != g_wifiGen.load(); };
    if (cancelled()) return;

    std::vector<WifiNet> nets;
    DWORD firstError = ERROR_SUCCESS;
    bool hadAdapter = false, hadSuccess = false;
    WlanHandle wlan;
    DWORD error = wlan.Open();
    if (error != ERROR_SUCCESS) {
        FinishWifi(gen, notify, msg, {}, error, false);
        return;
    }
    WlanMemory interfaces;
    error = WlanEnumInterfaces(wlan.value, nullptr, reinterpret_cast<PWLAN_INTERFACE_INFO_LIST*>(&interfaces.value));
    auto list = interfaces.as<WLAN_INTERFACE_INFO_LIST>();
    if (error != ERROR_SUCCESS) {
        FinishWifi(gen, notify, msg, {}, error, false);
        return;
    }
    if (!list || !list->dwNumberOfItems) {
        FinishWifi(gen, notify, msg, {}, ERROR_NOT_FOUND, false);
        return;
    }
    hadAdapter = true;
    {   // Phase A: the OS keeps its own scan results (it rescans on its own); publish them now so the page has rows
        // while the real scan below runs. The old code showed nothing until WlanScan had completed (1-4 s).
        std::vector<WifiNet> early;
        bool any = false;
        for (DWORD i = 0; i < list->dwNumberOfItems && !cancelled(); ++i)
            if (ReadWifiList(wlan.value, list->InterfaceInfo[i].InterfaceGuid, early) == ERROR_SUCCESS) any = true;
        if (any && !cancelled()) PublishWifiPartial(gen, notify, msg, std::move(early));
    }
    {
        std::lock_guard lock(g_scanRegistryMutex);
        g_scanRegistry[signal.get()] = signal;
    }
    DWORD previousSources = 0;
    error = WlanRegisterNotification(wlan.value, WLAN_NOTIFICATION_SOURCE_ACM, TRUE, WlanNotify,
                                     signal.get(), nullptr, &previousSources);
    if (error != ERROR_SUCCESS) {
        { std::lock_guard lock(g_scanRegistryMutex); g_scanRegistry.erase(signal.get()); }
        FinishWifi(gen, notify, msg, {}, error, hadAdapter);
        return;
    }
    WlanNotificationRegistration registration{wlan.value, signal, true};

    for (DWORD i = 0; i < list->dwNumberOfItems && !cancelled(); ++i) {
        auto const& iface = list->InterfaceInfo[i];
        {
            std::lock_guard lock(signal->mutex);
            signal->interfaceId = iface.InterfaceGuid;
            signal->complete = false;
            signal->cancelled = false;
            signal->result = ERROR_SUCCESS;
        }
        error = WlanScan(wlan.value, &iface.InterfaceGuid, nullptr, nullptr, nullptr);
        DWORD scanError = error;
        if (error == ERROR_SUCCESS) {
            std::unique_lock lock(signal->mutex);
            bool ended = signal->changed.wait_for(lock, std::chrono::seconds(4), [&] {
                return signal->complete || signal->cancelled || cancelled();
            });
            if (signal->cancelled || cancelled()) break;
            scanError = ended ? signal->result : ERROR_TIMEOUT;
        }
        // Read the list whether or not the scan finished in time: a slow or throttled scan still leaves the
        // OS's latest results, and those are better than losing the rows phase A already showed.
        const DWORD status = ReadWifiList(wlan.value, iface.InterfaceGuid, nets);
        if (status == ERROR_SUCCESS) {
            hadSuccess = true;
            error = scanError;  // kept as a diagnostic: FinishWifi reports Ready when any adapter delivered rows
        } else {
            error = scanError != ERROR_SUCCESS ? scanError : status;
        }
        if (error != ERROR_SUCCESS && firstError == ERROR_SUCCESS) firstError = error;
    }

    registration.Reset();
    if (!cancelled()) {
        DWORD result = hadSuccess ? firstError : firstError == ERROR_SUCCESS ? ERROR_GEN_FAILURE : firstError;
        CompleteWifi(gen, notify, msg, std::move(nets), result, hadAdapter, hadSuccess);
    }
}

void WifiScanTask(unsigned gen, HWND notify, UINT msg, std::shared_ptr<ScanSignal> signal) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (winrt::hresult_error const& e) {
        FinishWifi(gen, notify, msg, {}, static_cast<DWORD>(e.code()), false);
        return;
    } catch (...) {
        FinishWifi(gen, notify, msg, {}, ERROR_GEN_FAILURE, false);
        return;
    }
    struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
    try {
        WifiScanTaskBody(gen, notify, msg, std::move(signal));
    } catch (winrt::hresult_error const& e) {
        FinishWifi(gen, notify, msg, {}, static_cast<DWORD>(e.code()), true);
    } catch (...) {
        FinishWifi(gen, notify, msg, {}, ERROR_GEN_FAILURE, true);
    }
}

// ---- Bluetooth: one long-lived watcher pair over AssociationEndpoints --------------------------------------------
// Classic (BluetoothDevice) and LE (BluetoothLEDevice) paired devices are separate selectors; the old code watched
// Classic only. Connection state comes from System.Devices.Aep.IsConnected, requested with the watcher, so it needs
// no BluetoothDevice object per device (the old code opened every paired device with FromIdAsync, again on every
// Updated event). Entries of one physical device (Classic + LE) share a ContainerId and are merged into one row.
namespace wde = winrt::Windows::Devices::Enumeration;
namespace wdb = winrt::Windows::Devices::Bluetooth;

struct BtRec {
    std::wstring name, group;
    bool connected = false, classic = false;
    wde::DeviceInformation info{nullptr};
};
struct BtTransport {
    wde::DeviceWatcher watcher{nullptr};
    wde::DeviceWatcher::Added_revoker addedRev;
    wde::DeviceWatcher::Updated_revoker updatedRev;
    wde::DeviceWatcher::Removed_revoker removedRev;
    wde::DeviceWatcher::EnumerationCompleted_revoker completedRev;
    wde::DeviceWatcher::Stopped_revoker stoppedRev;
    bool running = false, enumerated = false;
};
struct BtHub {
    std::mutex m;
    std::unordered_map<std::wstring, BtRec> recs;  // by AEP id
    std::vector<BtDevice> list;                    // the published snapshot
    DiscoveryStatus status;
    ULONGLONG updated = 0;
    bool sawConnectionProperty = false, workerBusy = false;
    BtTransport t[2];  // 0 = Classic, 1 = LE
    std::unordered_set<std::wstring> connecting;  // NormId of the devices with a connect/disconnect in flight
    Coalescer post;
};
BtHub& Bt() {
    static BtHub* hub = new BtHub;  // never destroyed: watcher callbacks may still run at process exit
    return *hub;
}
void BtNoteAudioChanged() { Bt().post.Poke(); }

auto BtProps() {
    return winrt::single_threaded_vector<winrt::hstring>({L"System.Devices.Aep.IsConnected", L"System.Devices.Aep.ContainerId", L"System.Devices.Aep.DeviceAddress"});
}
winrt::hstring BtSelector(bool classic) {
    return classic ? wdb::BluetoothDevice::GetDeviceSelectorFromPairingState(true)
                   : wdb::BluetoothLEDevice::GetDeviceSelectorFromPairingState(true);
}

// Caller holds h.m.
void FillBt(BtHub& h, BtRec& r, wde::DeviceInformation const& info, bool classic) {
    r.info = info;
    r.classic = classic;
    r.name = info.Name().c_str();
    auto props = info.Properties();
    if (auto conn = props.TryLookup(L"System.Devices.Aep.IsConnected")) {
        h.sawConnectionProperty = true;
        r.connected = winrt::unbox_value_or<bool>(conn, false);
    }
    winrt::guid container = winrt::unbox_value_or<winrt::guid>(props.TryLookup(L"System.Devices.Aep.ContainerId"), winrt::guid{});
    std::wstring address = winrt::unbox_value_or<winrt::hstring>(props.TryLookup(L"System.Devices.Aep.DeviceAddress"), winrt::hstring{}).c_str();
    for (auto& c : address) c = static_cast<wchar_t>(towlower(c));
    if (container != winrt::guid{}) r.group = winrt::to_hstring(container).c_str();
    else if (!address.empty()) r.group = address;
    else r.group = info.Id().c_str();
}

// Caller holds h.m. Builds the merged, ordered snapshot; true if it differs from the published one.
bool RebuildBt(BtHub& h) {
    std::unordered_map<std::wstring, BtDevice> merged;
    for (auto const& [id, r] : h.recs) {
        auto& d = merged[r.group];
        d.id = r.group;
        d.connected = d.connected || r.connected;
        if (r.classic || d.aepId.empty()) d.aepId = id;
        if (!r.name.empty() && (d.name.empty() || r.classic)) d.name = r.name;
    }
    std::vector<BtDevice> next;
    for (auto& [_, d] : merged) if (!d.name.empty()) next.push_back(std::move(d));
    std::sort(next.begin(), next.end(), [](BtDevice const& a, BtDevice const& b) {
        if (a.connected != b.connected) return a.connected > b.connected;
        if (int c = CompareNoCase(a.name, b.name)) return c < 0;
        return a.id < b.id;
    });
    bool same = next.size() == h.list.size();
    for (size_t i = 0; same && i < next.size(); ++i)
        same = next[i].id == h.list[i].id && next[i].name == h.list[i].name && next[i].connected == h.list[i].connected;
    if (same) return false;
    h.list = std::move(next);
    h.updated = GetTickCount64();
    return true;
}

void BtAdded(bool classic, wde::DeviceInformation const& info) {
    auto& h = Bt();
    bool changed;
    {
        std::lock_guard lock(h.m);
        FillBt(h, h.recs[info.Id().c_str()], info, classic);
        changed = RebuildBt(h);
    }
    if (changed) h.post.Poke();
}
void BtUpdated(bool classic, wde::DeviceInformationUpdate const& update) {
    auto& h = Bt();
    bool changed;
    {
        std::lock_guard lock(h.m);
        auto it = h.recs.find(update.Id().c_str());
        if (it == h.recs.end()) return;
        it->second.info.Update(update);
        FillBt(h, it->second, it->second.info, classic);
        changed = RebuildBt(h);
    }
    if (changed) h.post.Poke();
}
void BtRemoved(wde::DeviceInformationUpdate const& update) {
    auto& h = Bt();
    bool changed;
    {
        std::lock_guard lock(h.m);
        h.recs.erase(update.Id().c_str());
        changed = RebuildBt(h);
    }
    if (changed) h.post.Poke();
}

// Worker thread only. Throws on failure; the caller decides what that means for the status.
void StartBtTransport(BtHub& h, int i) {
    auto& t = h.t[i];
    const bool classic = i == 0;
    t.addedRev.revoke();
    t.updatedRev.revoke();
    t.removedRev.revoke();
    t.completedRev.revoke();
    t.stoppedRev.revoke();
    t.watcher = wde::DeviceInformation::CreateWatcher(BtSelector(classic), BtProps(), wde::DeviceInformationKind::AssociationEndpoint);
    t.addedRev = t.watcher.Added(winrt::auto_revoke, [classic](auto const&, wde::DeviceInformation const& info) { BtAdded(classic, info); });
    t.updatedRev = t.watcher.Updated(winrt::auto_revoke, [classic](auto const&, wde::DeviceInformationUpdate const& u) { BtUpdated(classic, u); });
    t.removedRev = t.watcher.Removed(winrt::auto_revoke, [](auto const&, wde::DeviceInformationUpdate const& u) { BtRemoved(u); });
    t.completedRev = t.watcher.EnumerationCompleted(winrt::auto_revoke, [i](auto const&, auto const&) {
        std::lock_guard lock(Bt().m);
        Bt().t[i].enumerated = true;
    });
    t.stoppedRev = t.watcher.Stopped(winrt::auto_revoke, [i](auto const&, auto const&) {  // aborted: the next refresh restarts it
        std::lock_guard lock(Bt().m);
        Bt().t[i].running = false;
    });
    {
        std::lock_guard lock(h.m);
        t.running = true;
        t.enumerated = false;
    }
    try {
        t.watcher.Start();
    } catch (...) {
        std::lock_guard lock(h.m);
        t.running = false;
        throw;
    }
}

// Re-reads both transports with FindAllAsync (bounded) and reconciles the cache: a safety net for a missed event.
// Returns true if every transport answered in time.
bool ReconcileBt(BtHub& h) {
    namespace wf = winrt::Windows::Foundation;
    bool allAnswered = true;
    for (int i = 0; i < 2; ++i) {
        const bool classic = i == 0;
        std::vector<wde::DeviceInformation> fresh;
        try {
            auto op = wde::DeviceInformation::FindAllAsync(BtSelector(classic), BtProps(), wde::DeviceInformationKind::AssociationEndpoint);
            if (op.wait_for(std::chrono::seconds(4)) != wf::AsyncStatus::Completed) {
                op.Cancel();
                allAnswered = false;
                continue;
            }
            for (auto const& info : op.GetResults()) fresh.push_back(info);
        } catch (...) {
            allAnswered = false;
            continue;
        }
        std::lock_guard lock(h.m);
        std::unordered_map<std::wstring, bool> seen;
        for (auto const& info : fresh) {
            std::wstring id = info.Id().c_str();
            seen[id] = true;
            FillBt(h, h.recs[id], info, classic);
        }
        for (auto it = h.recs.begin(); it != h.recs.end();) {
            if (it->second.classic == classic && !seen.count(it->first)) it = h.recs.erase(it);
            else ++it;
        }
        RebuildBt(h);
    }
    return allAnswered;
}

// Only if the AEP never reported IsConnected: ask each Classic device object once, then release it.
void ProbeBtConnection(BtHub& h) {
    namespace wf = winrt::Windows::Foundation;
    std::vector<std::wstring> ids;
    {
        std::lock_guard lock(h.m);
        if (h.sawConnectionProperty) return;
        for (auto const& [id, r] : h.recs) if (r.classic) ids.push_back(id);
    }
    for (auto const& id : ids) {
        try {
            auto op = wdb::BluetoothDevice::FromIdAsync(id);
            if (op.wait_for(std::chrono::seconds(2)) != wf::AsyncStatus::Completed) { op.Cancel(); continue; }
            auto device = op.GetResults();
            if (!device) continue;
            const bool connected = device.ConnectionStatus() == wdb::BluetoothConnectionStatus::Connected;
            device.Close();
            std::lock_guard lock(h.m);
            auto it = h.recs.find(id);
            if (it != h.recs.end()) it->second.connected = connected;
        } catch (...) {}
    }
    std::lock_guard lock(h.m);
    RebuildBt(h);
}

void BtWorker() {
    auto& h = Bt();
    DiscoveryStatus result{DiscoveryPhase::Ready, ERROR_SUCCESS};
    try {
        bool wasRunning;
        {
            std::lock_guard lock(h.m);
            wasRunning = h.t[0].running && h.t[1].running;
        }
        HRESULT firstFailure = S_OK;
        int started = 0;
        for (int i = 0; i < 2; ++i) {
            bool running;
            { std::lock_guard lock(h.m); running = h.t[i].running; }
            if (running) { ++started; continue; }
            try {
                StartBtTransport(h, i);
                ++started;
            } catch (winrt::hresult_error const& e) {
                if (firstFailure == S_OK) firstFailure = e.code();
            } catch (...) {
                if (firstFailure == S_OK) firstFailure = E_FAIL;
            }
        }
        if (!started) {
            result = {firstFailure == E_ACCESSDENIED ? DiscoveryPhase::AccessDenied : DiscoveryPhase::Failed, static_cast<DWORD>(firstFailure)};
        } else if (wasRunning) {
            if (!ReconcileBt(h)) {
                std::lock_guard lock(h.m);
                result = {h.list.empty() ? DiscoveryPhase::Failed : DiscoveryPhase::Ready, ERROR_TIMEOUT};
            }
        } else {
            for (int waited = 0; waited < 8000; waited += 50) {  // first enumeration: rows stream in through the watchers
                bool done = true;
                {
                    std::lock_guard lock(h.m);
                    for (auto& t : h.t) if (t.running && !t.enumerated) done = false;
                }
                if (done) break;
                Sleep(50);
            }
        }
        if (started) ProbeBtConnection(h);
    } catch (winrt::hresult_error const& e) {
        result = {e.code() == E_ACCESSDENIED ? DiscoveryPhase::AccessDenied : DiscoveryPhase::Failed, static_cast<DWORD>(e.code())};
    } catch (...) {
        result = {DiscoveryPhase::Failed, ERROR_GEN_FAILURE};
    }
    {
        std::lock_guard lock(h.m);
        h.status = result;
        h.workerBusy = false;
        if (result.phase == DiscoveryPhase::Ready) h.updated = GetTickCount64();
    }
    h.post.PostNow();
}

}  // namespace

void WifiRefresh(HWND notify, UINT msg) {
    auto signal = std::make_shared<ScanSignal>();
    unsigned gen;
    {
        std::lock_guard lock(g_wifiMutex);
        g_wifiNotify = notify;
        g_wifiMsg = msg;
        if (g_activeWifiScan.lock()) {
            // A scan is already running: join it instead of cancelling and restarting, so a user who presses
            // Refresh repeatedly does not stack scans (the old code cancelled and restarted on every press).
            gen = 0;
        } else {
            gen = ++g_wifiGen;
            g_activeWifiScan = signal;
            g_wifiStatus = {DiscoveryPhase::Loading, ERROR_SUCCESS};  // the cached rows stay: the page opens with them
        }
    }
    // Immediately: the page re-reads the cache (and the Loading status) on its next message.
    if (notify) PostMessageW(notify, msg, 0, 0);
    if (!gen) return;
    try {
        std::thread(WifiScanTask, gen, notify, msg, std::move(signal)).detach();
    } catch (...) {
        {
            std::lock_guard lock(g_wifiMutex);
            if (gen == g_wifiGen.load()) g_wifiStatus = {DiscoveryPhase::Failed, ERROR_NOT_ENOUGH_MEMORY};
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
    }
}

void WifiStop() {
    ++g_wifiGen;
    std::shared_ptr<ScanSignal> active;
    {
        std::lock_guard lock(g_wifiMutex);
        active = g_activeWifiScan.lock();
        g_activeWifiScan.reset();
        ClearWifiPendingLocked();  // the cached rows stay for the next open
        g_wifiStatus = {};
        g_wifiNotify = nullptr;
        g_wifiMsg = 0;
    }
    if (active) {
        std::lock_guard lock(active->mutex);
        active->cancelled = true;
        active->changed.notify_all();
    }
}

std::vector<WifiNet> WifiNets() {
    std::lock_guard lock(g_wifiMutex);
    return g_wifiNets;
}
DiscoveryStatus WifiStatus() {
    std::lock_guard lock(g_wifiMutex);
    return g_wifiStatus;
}

bool WifiConnect(WifiNet const& network) {
    WifiNet net = network;
    unsigned generation;
    HWND notify;
    UINT msg;
    {
        std::lock_guard lock(g_wifiMutex);
        generation = g_wifiGen.load();
        notify = g_wifiNotify;
        msg = g_wifiMsg;
    }
    if (DryRun()) {
        Log(radio_policy::HasSavedProfile(net.profileName.c_str())
                ? L"DRYRUN join Wi-Fi profile " + net.profileName
                : L"DRYRUN open Windows Wi-Fi Settings for unprofiled network");
        return true;
    }
    // Everything below is a blocking call (WlanOpenHandle, WlanConnect, ShellExecute): it runs on a worker. The row is
    // marked pending and the status says Connecting at once, so the page can draw the state before Windows answers.
    const bool profiled = radio_policy::HasSavedProfile(net.profileName.c_str());
    if (profiled) {
        {
            std::lock_guard lock(g_wifiMutex);
            if (generation == g_wifiGen.load()) {
                g_wifiStatus = {DiscoveryPhase::Connecting, ERROR_SUCCESS};
                for (auto& row : g_wifiNets)
                    if (!net.id.empty() ? row.id == net.id : row.ssid == net.ssid && row.interfaceId == net.interfaceId) row.pending = true;
            }
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
    }
    try {
        std::thread([net = std::move(net), generation, notify, msg, profiled]() mutable {
            auto fail = [&](DiscoveryPhase phase, DWORD error) {
                {
                    std::lock_guard lock(g_wifiMutex);
                    if (generation == g_wifiGen.load()) { g_wifiStatus = {phase, error}; ClearWifiPendingLocked(); }
                }
                if (notify) PostMessageW(notify, msg, 0, 0);
            };
            if (!profiled) {
                const bool co = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
                HINSTANCE result = ShellExecuteW(nullptr, L"open", L"ms-settings:network-wifi", nullptr, nullptr, SW_SHOWNORMAL);
                if (co) CoUninitialize();
                if (reinterpret_cast<INT_PTR>(result) <= 32) fail(DiscoveryPhase::Failed, ERROR_GEN_FAILURE);
                return;
            }
            WlanHandle wlan;
            DWORD error = wlan.Open();
            if (error == ERROR_SUCCESS) {
                WLAN_CONNECTION_PARAMETERS cp{};
                cp.wlanConnectionMode = wlan_connection_mode_profile;
                cp.strProfile = net.profileName.c_str();
                cp.dot11BssType = dot11_BSS_type_any;
                error = WlanConnect(wlan.value, &net.interfaceId, &cp, nullptr);
            }
            if (error != ERROR_SUCCESS) { fail(radio_policy::PhaseForWlanError(error), error); return; }
            WifiConnectionReadback(generation, notify, msg, std::move(net.profileName), net.interfaceId);
        }).detach();
    } catch (...) {
        {
            std::lock_guard lock(g_wifiMutex);
            if (generation == g_wifiGen.load()) { g_wifiStatus = {DiscoveryPhase::Failed, ERROR_NOT_ENOUGH_MEMORY}; ClearWifiPendingLocked(); }
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
        return false;
    }
    return true;
}

ListInfo WifiInfo() {
    std::lock_guard lock(g_wifiMutex);
    const ULONGLONG stamp = g_wifiStamp.load();
    return {stamp != 0, g_wifiStatus.phase == DiscoveryPhase::Loading, stamp};
}


void BtRefresh(HWND notify, UINT msg) {
    auto& h = Bt();
    h.post.SetTarget(notify, msg);
    Out().listeners |= 2;  // the rows' audio facts follow the endpoints
    OutputsKick();
    bool start;
    {
        std::lock_guard lock(h.m);
        start = !h.workerBusy;  // a refresh already running is joined, not stacked
        if (start) {
            h.workerBusy = true;
            h.status = {DiscoveryPhase::Loading, ERROR_SUCCESS};  // the cached rows stay
        }
    }
    h.post.PostNow();  // the page re-reads the cache (and the Loading status) on its next message
    if (start && !RunOffThread(BtWorker)) {
        {
            std::lock_guard lock(h.m);
            h.workerBusy = false;
            h.status = {DiscoveryPhase::Failed, ERROR_NOT_ENOUGH_MEMORY};
        }
        h.post.PostNow();
    }
}

// The watchers keep running and keep the cache current; only the notifications stop.
void BtStop() {
    Out().listeners &= ~2;
    Bt().post.SetTarget(nullptr, 0);
}

std::vector<BtDevice> BtDevices() {
    auto& h = Bt();
    std::vector<BtDevice> list;
    std::unordered_set<std::wstring> connecting;
    {
        std::lock_guard lock(h.m);
        list = h.list;
        connecting = h.connecting;
    }
    auto& out = Out();
    std::lock_guard lock(out.m);
    for (auto& d : list) {
        const std::wstring key = NormId(d.id);
        d.pending = connecting.count(key) != 0;
        std::vector<EpInfo const*> mine;
        for (auto const& ep : out.all) if (!ep.container.empty() && ep.container == key) mine.push_back(&ep);
        if (mine.empty()) continue;
        d.audio = true;
        if (auto const* active = BestEndpoint(mine, true)) {
            d.audioActive = true;
            d.audioDefault = active->isDefault;
            d.endpointId = active->id;
        } else if (auto const* any = BestEndpoint(mine, false)) {
            d.endpointId = any->id;
        }
    }
    return list;
}
DiscoveryStatus BtStatus() {
    auto& h = Bt();
    std::lock_guard lock(h.m);
    return h.status;
}
ListInfo BtInfo() {
    auto& h = Bt();
    std::lock_guard lock(h.m);
    return {h.updated != 0, h.status.phase == DiscoveryPhase::Loading, h.updated};
}

// ---- Bluetooth audio: connect / disconnect through the audio driver's one-shot properties ---------------------------
// Windows has no public "connect this paired device" call for desktop apps. The Bluetooth audio drivers implement
// KSPROPERTY_ONESHOT_RECONNECT / _DISCONNECT on their KS filter (Microsoft Learn: "prompt the audio driver to attempt
// to connect to the Bluetooth audio device"; Get-only, target Filter, no value, success only means the driver tried);
// that is what the Sound control panel's Connect command uses. Reaching the filter from an endpoint:
//   IMMDevice(endpoint) -> Activate(IDeviceTopology) -> IConnector -> GetDeviceIdConnectedTo() -> IMMDeviceEnumerator::
//   GetDevice(that id) -> Activate(IKsControl) -> KsProperty({KSPROPSETID_BtAudio, id, GET}).
// Constants: KSPROPSETID_BtAudio {7FA06C40-B8F6-4C7E-8556-E8C33A12E54D}, KSPROPERTY_ONESHOT_RECONNECT = 0, _DISCONNECT = 1
// (microsoft/windows-rs metadata/win32/ksmedia.rdl, generated from the SDK headers; MicrosoftDocs kspropsetid-btaudio.md).
// The sequence was cross-checked against two independent open implementations (read for the approach only; this code
// is written fresh).
namespace {
constexpr GUID kKsetBtAudio = {0x7fa06c40, 0xb8f6, 0x4c7e, {0x85, 0x56, 0xe8, 0xc3, 0x3a, 0x12, 0xe5, 0x4d}};
constexpr ULONG kBtOneShotReconnect = 0, kBtOneShotDisconnect = 1;

bool SendKsBtProperty(IMMDeviceEnumerator* en, wchar_t const* filterDeviceId, ULONG propId) {
    winrt::com_ptr<IMMDevice> dev;
    if (FAILED(en->GetDevice(filterDeviceId, dev.put()))) return false;
    winrt::com_ptr<IKsControl> ks;
    if (FAILED(dev->Activate(__uuidof(IKsControl), CLSCTX_ALL, nullptr, ks.put_void()))) return false;
    KSPROPERTY prop{};
    prop.Set = kKsetBtAudio;
    prop.Id = propId;
    prop.Flags = KSPROPERTY_TYPE_GET;
    ULONG returned = 0;
    return SUCCEEDED(ks->KsProperty(&prop, sizeof(prop), nullptr, 0, &returned));
}

// One endpoint -> its Bluetooth filter -> the one-shot. True if the driver accepted a request.
bool SendBtOneShot(IMMDeviceEnumerator* en, std::wstring const& endpointId, ULONG propId) {
    winrt::com_ptr<IMMDevice> endpoint;
    if (FAILED(en->GetDevice(endpointId.c_str(), endpoint.put()))) return false;
    winrt::com_ptr<IDeviceTopology> topo;
    if (FAILED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr, topo.put_void()))) return false;
    UINT count = 0;
    if (FAILED(topo->GetConnectorCount(&count))) return false;
    for (UINT i = 0; i < count; ++i) {
        winrt::com_ptr<IConnector> conn;
        if (FAILED(topo->GetConnector(i, conn.put()))) continue;
        LPWSTR other = nullptr;
        if (SUCCEEDED(conn->GetDeviceIdConnectedTo(&other)) && other) {
            std::wstring id = other;
            CoTaskMemFree(other);
            if (SendKsBtProperty(en, id.c_str(), propId)) return true;
        }
        // Second route to the same filter: the connected connector's part -> its topology object -> device id.
        winrt::com_ptr<IConnector> peer;
        if (FAILED(conn->GetConnectedTo(peer.put()))) continue;
        auto part = peer.try_as<IPart>();
        winrt::com_ptr<IDeviceTopology> peerTopo;
        LPWSTR peerId = nullptr;
        // (mingw-w64's header spells IPart::GetTopologyObject with an s; it is the same vtable slot.)
        if (part && SUCCEEDED(part->GetTopologyObjects(peerTopo.put())) && SUCCEEDED(peerTopo->GetDeviceId(&peerId)) && peerId) {
            std::wstring id = peerId;
            CoTaskMemFree(peerId);
            if (SendKsBtProperty(en, id.c_str(), propId)) return true;
        }
    }
    return false;
}

bool AnyActive(std::vector<EpInfo> const& all, std::wstring const& container) {
    for (auto const& ep : all) if (ep.container == container && ep.state == DEVICE_STATE_ACTIVE) return true;
    return false;
}

// Runs on a worker. Returns the result; the caller does the bookkeeping.
AudioOpResult RunBtAudio(std::wstring const& container, bool connect) {
    const AudioOpResult kSettings = AudioOpResult::FellBackToSettings;
    auto openSettings = [&] {
        OpenSettings(L"ms-settings:bluetooth");
        return kSettings;
    };
    if (DryRun()) { Log(std::wstring(L"DRYRUN bluetooth audio ") + (connect ? L"connect " : L"disconnect ") + container); return AudioOpResult::Succeeded; }
    if (container.empty()) return connect ? openSettings() : AudioOpResult::Failed;
    winrt::com_ptr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), en.put_void())))
        return connect ? openSettings() : AudioOpResult::Failed;
    auto all = AllRenderEndpoints();
    std::vector<EpInfo const*> mine;
    for (auto const& ep : all) if (ep.container == container) mine.push_back(&ep);
    if (mine.empty()) return connect ? openSettings() : AudioOpResult::Failed;  // not an audio device Windows knows

    const bool wasActive = AnyActive(all, container);
    if (connect == wasActive && connect) {
        // already connected for audio: only the default output is left to do
    } else if (connect == wasActive) {
        // asked to disconnect something that is not connected: nothing to do
        return AudioOpResult::Succeeded;
    } else {
        int sent = 0;
        for (auto const* ep : mine)
            if (SendBtOneShot(en.get(), ep->id, connect ? kBtOneShotReconnect : kBtOneShotDisconnect)) ++sent;
        if (!sent) return connect ? openSettings() : AudioOpResult::Failed;
        bool settled = false;
        for (int waited = 0; waited < 10000 && !settled; waited += 250) {
            Sleep(250);
            settled = AnyActive(AllRenderEndpoints(), container) == connect;
        }
        if (!settled) return connect ? openSettings() : AudioOpResult::Failed;
    }
    if (!connect) return AudioOpResult::Succeeded;
    // Connected for audio: make the stereo endpoint the default output (unless it already is).
    all = AllRenderEndpoints();
    mine.clear();
    for (auto const& ep : all) if (ep.container == container) mine.push_back(&ep);
    if (auto const* best = BestEndpoint(mine, true)) {
        if (!best->isDefault) return SetDefaultNow(best->id);
    }
    return AudioOpResult::Succeeded;
}

void BtAudioOp(std::wstring const& deviceId, bool connect, HWND notify, UINT msg, std::function<void(AudioOpResult)> done) {
    auto& h = Bt();
    const std::wstring key = NormId(deviceId);
    {
        std::lock_guard lock(h.m);
        if (!h.connecting.insert(key).second) return;  // one at a time per device
    }
    h.post.PostNow();  // the row shows "Connecting…" at once
    const bool started = RunOffThread([key, connect, notify, msg, done] {
        AudioOpResult result = AudioOpResult::Failed;
        try { result = RunBtAudio(key, connect); } catch (...) {}
        {
            auto& hub = Bt();
            std::lock_guard lock(hub.m);
            hub.connecting.erase(key);
        }
        if (done) done(result);
        OutputsKick();
        Bt().post.PostNow();
        if (notify) PostMessageW(notify, msg, 0, 0);
    });
    if (!started) {
        {
            std::lock_guard lock(h.m);
            h.connecting.erase(key);
        }
        h.post.PostNow();
        if (done) done(AudioOpResult::Failed);
    }
}
}  // namespace

void ConnectBluetoothAudio(std::wstring const& btDeviceId, HWND notify, UINT msg, std::function<void(AudioOpResult)> done) {
    BtAudioOp(btDeviceId, true, notify, msg, std::move(done));
}
void DisconnectBluetoothAudio(std::wstring const& btDeviceId, HWND notify, UINT msg, std::function<void(AudioOpResult)> done) {
    BtAudioOp(btDeviceId, false, notify, msg, std::move(done));
}

// ---- Now Playing ------------------------------------------------------------------------------
void Media::Start(HWND notify, UINT event, std::function<void()> changed) {
    Stop();
    m_notify = notify;
    m_event = event;
    m_changed = std::move(changed);
    Init(m_gen, *m_gen);
}

void Media::Stop() {
    ++*m_gen;
    m_mgrRev.revoke();
    m_pbRev.revoke();
    m_propRev.revoke();
    m_mgr = nullptr;
    m_sess = nullptr;
    m_state = {};
    m_changed = nullptr;
}

winrt::fire_and_forget Media::Init(std::shared_ptr<unsigned> live, unsigned gen) {
    try {
        auto mgr = co_await Manager::RequestAsync();
        if (gen != *live) co_return;
        m_mgr = mgr;
        HWND h = m_notify;
        UINT e = m_event;
        m_mgrRev = mgr.CurrentSessionChanged(winrt::auto_revoke, [h, e](auto&&, auto&&) { PostMessageW(h, e, 0, 0); });
        PullAsync();
    } catch (...) {}
}

namespace {
// Decodes image bytes to a 96x96 premultiplied BGRA square (scaled to cover, centre-cropped).
void DecodeArt(std::vector<uint8_t> const& bytes, MediaState& out) {
    constexpr UINT kSize = 96;
    winrt::com_ptr<IWICImagingFactory> f;
    winrt::com_ptr<IWICStream> st;
    winrt::com_ptr<IWICBitmapDecoder> dec;
    winrt::com_ptr<IWICBitmapFrameDecode> fr;
    if (bytes.empty() || FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(f.put()))) ||
        FAILED(f->CreateStream(st.put())) ||
        FAILED(st->InitializeFromMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()))) ||
        FAILED(f->CreateDecoderFromStream(st.get(), nullptr, WICDecodeMetadataCacheOnDemand, dec.put())) ||
        FAILED(dec->GetFrame(0, fr.put())))
        return;
    UINT w = 0, h = 0;
    fr->GetSize(&w, &h);
    if (!w || !h) return;
    double k = std::max(double(kSize) / w, double(kSize) / h);
    UINT sw = std::max<UINT>(kSize, static_cast<UINT>(w * k + 0.5)), sh = std::max<UINT>(kSize, static_cast<UINT>(h * k + 0.5));
    winrt::com_ptr<IWICBitmapScaler> sc;
    winrt::com_ptr<IWICFormatConverter> cv;
    if (FAILED(f->CreateBitmapScaler(sc.put())) || FAILED(sc->Initialize(fr.get(), sw, sh, WICBitmapInterpolationModeFant)) ||
        FAILED(f->CreateFormatConverter(cv.put())) ||
        FAILED(cv->Initialize(sc.get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return;
    WICRect r{static_cast<INT>((sw - kSize) / 2), static_cast<INT>((sh - kSize) / 2), static_cast<INT>(kSize), static_cast<INT>(kSize)};
    out.art.assign(kSize * kSize, 0);
    if (SUCCEEDED(cv->CopyPixels(&r, kSize * 4, kSize * kSize * 4, reinterpret_cast<BYTE*>(out.art.data())))) out.artSize = kSize;
    else out.art.clear();
}
}  // namespace

winrt::fire_and_forget Media::PullAsync() {
    auto live = m_gen, newest = m_seq;
    const unsigned gen = *live, seq = ++*newest;
    if (!m_mgr) co_return;
    try {
        auto sess = m_mgr.GetCurrentSession();
        if (sess != m_sess) {
            m_pbRev.revoke();
            m_propRev.revoke();
            m_sess = sess;
            if (sess) {
                HWND h = m_notify;
                UINT e = m_event;
                m_pbRev = sess.PlaybackInfoChanged(winrt::auto_revoke, [h, e](auto&&, auto&&) { PostMessageW(h, e, 0, 0); });
                m_propRev = sess.MediaPropertiesChanged(winrt::auto_revoke, [h, e](auto&&, auto&&) { PostMessageW(h, e, 0, 0); });
            }
        }
        MediaState next;
        if (sess) {
            next.has = true;
            next.source = sess.SourceAppUserModelId().c_str();
            // Some sessions hand back a null playback info / controls / properties object (S_OK + nullptr); the
            // projection does not check, so dereferencing one is an access violation that catch(...) cannot catch.
            if (auto info = sess.GetPlaybackInfo()) {
                next.playing = info.PlaybackStatus() ==
                               winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                if (auto c = info.Controls()) {
                    next.canPrev = c.IsPreviousEnabled();
                    next.canNext = c.IsNextEnabled();
                }
            }
            auto props = co_await sess.TryGetMediaPropertiesAsync();
            if (gen != *live || seq != *newest) co_return;
            if (!props) {
                m_state = std::move(next);
                if (m_changed) m_changed();
                co_return;
            }
            next.title = props.Title().c_str();
            next.artist = props.Artist().c_str();
            if (next.source == m_state.source && next.title == m_state.title && next.artist == m_state.artist) {
                next.art = std::move(m_state.art);  // transfer the cached bitmap, no allocation/copy on playback events
                next.artSize = m_state.artSize;
            } else if (auto thumb = props.Thumbnail()) {
                try {
                    auto stream = co_await thumb.OpenReadAsync();
                    const auto size = static_cast<uint32_t>(stream.Size());
                    if (size && size < (8u << 20)) {  // a huge or empty thumbnail is not worth loading on the UI thread
                        winrt::Windows::Storage::Streams::DataReader reader(stream);
                        co_await reader.LoadAsync(size);
                        std::vector<uint8_t> bytes(size);
                        reader.ReadBytes(bytes);
                        DecodeArt(bytes, next);
                    }
                } catch (...) {}
                if (gen != *live || seq != *newest) co_return;
            }
        }
        if (gen != *live || seq != *newest) co_return;
        m_state = std::move(next);
        if (m_changed) m_changed();
    } catch (...) {}
}

void Media::PlayPause() {
    if (DryRun()) return Log(L"DRYRUN media play/pause");
    try { if (m_sess) m_sess.TryTogglePlayPauseAsync(); } catch (...) {}  // the player may have just quit
}
void Media::Next() {
    if (DryRun()) return Log(L"DRYRUN media next");
    try { if (m_sess) m_sess.TrySkipNextAsync(); } catch (...) {}
}
void Media::Prev() {
    if (DryRun()) return Log(L"DRYRUN media previous");
    try { if (m_sess) m_sess.TrySkipPreviousAsync(); } catch (...) {}
}

}  // namespace halo::svc
