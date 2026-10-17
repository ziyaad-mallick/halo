#include "services.h"

#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <wbemidl.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

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
    return true;
}

void AudioEndpoint::Unbind() {
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

bool SetDefaultOutput(std::wstring const& id) {
    if (DryRun()) { Log(L"DRYRUN default output -> " + id); return true; }
    winrt::com_ptr<IPolicyConfig> pc;
    if (SUCCEEDED(CoCreateInstance(kClsidPolicyConfig, nullptr, CLSCTX_ALL, kIidPolicyConfig, pc.put_void()))) {
        bool ok = true;
        for (ERole r : {eConsole, eMultimedia, eCommunications}) ok &= SUCCEEDED(pc->SetDefaultEndpoint(id.c_str(), r));
        if (ok) return true;
    }
    ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
    return false;
}

// ---- Power ----------------------------------------------------------------------------
Battery ReadBattery() {
    Battery b;
    SYSTEM_POWER_STATUS s{};
    if (GetSystemPowerStatus(&s)) {
        b.hasBattery = !(s.BatteryFlag & 128) && s.BatteryLifePercent <= 100;
        b.percent = s.BatteryLifePercent <= 100 ? s.BatteryLifePercent : 100;
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

std::atomic<int> g_level{-1};
std::atomic<bool> g_supported{false};
std::atomic<int> g_wanted{-1};
std::mutex g_mutex;  // one WMI conversation at a time
int g_applied = -2;

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

void Read() {
    if (auto svc = Connect())
        if (auto obj = First(svc.get(), L"WmiMonitorBrightness")) {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr)) && v.vt != VT_NULL) {
                g_level = v.bVal;
                g_supported = true;
            }
            VariantClear(&v);
        }
}

void Write(int pct) {
    auto svc = Connect();
    if (!svc) return;
    auto inst = First(svc.get(), L"WmiMonitorBrightnessMethods");
    if (!inst) return;
    VARIANT path;
    VariantInit(&path);
    BSTR cls = SysAllocString(L"WmiMonitorBrightnessMethods");
    winrt::com_ptr<IWbemClassObject> clsObj, inDef, in;
    HRESULT hr = svc->GetObject(cls, 0, nullptr, clsObj.put(), nullptr);
    SysFreeString(cls);
    if (SUCCEEDED(hr) && SUCCEEDED(inst->Get(L"__PATH", 0, &path, nullptr, nullptr)) && path.vt == VT_BSTR &&
        SUCCEEDED(clsObj->GetMethod(L"WmiSetBrightness", 0, inDef.put(), nullptr)) &&
        SUCCEEDED(inDef->SpawnInstance(0, in.put()))) {
        VARIANT t, b;
        VariantInit(&t);
        VariantInit(&b);
        t.vt = VT_I4; t.lVal = 1;
        b.vt = VT_I4; b.lVal = pct;
        in->Put(L"Timeout", 0, &t, 0);
        in->Put(L"Brightness", 0, &b, 0);
        BSTR method = SysAllocString(L"WmiSetBrightness");
        svc->ExecMethod(path.bstrVal, method, 0, nullptr, in.get(), nullptr, nullptr);
        SysFreeString(method);
    }
    VariantClear(&path);
}

}  // namespace

void Probe(HWND notify, UINT msg) {
    std::thread([notify, msg] {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            { std::lock_guard<std::mutex> lock(g_mutex); Read(); }
            CoUninitialize();
        }
        PostMessageW(notify, msg, 0, 0);
    }).detach();
}

bool Supported() { return g_supported; }
int Level() { return g_level; }

void Set(int percent) {
    percent = std::clamp(percent, 0, 100);
    g_level = percent;
    if (DryRun()) return Log(L"DRYRUN brightness -> " + std::to_wstring(percent));
    g_wanted = percent;
    std::thread([] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            int v = g_wanted;  // a newer thread may already have applied this value
            if (v != g_applied) { g_applied = v; Write(v); }
        }
        CoUninitialize();
    }).detach();
}

}  // namespace brightness

// ---- Radios ---------------------------------------------------------------------------------
namespace wdr = winrt::Windows::Devices::Radios;

void Radios::Start(HWND notify, UINT msg) {
    Stop();
    Find(notify, msg, m_gen);
}

void Radios::Stop() {
    m_gen++;
    m_wifiRev.revoke();
    m_btRev.revoke();
    m_wifi = nullptr;
    m_bt = nullptr;
}

winrt::fire_and_forget Radios::Find(HWND notify, UINT msg, unsigned gen) {
    try {
        auto radios = co_await wdr::Radio::GetRadiosAsync();  // resumes on the UI thread
        if (gen != m_gen) co_return;
        for (auto const& r : radios) {
            auto handler = [notify, msg](auto&&, auto&&) { PostMessageW(notify, msg, 0, 0); };
            if (r.Kind() == wdr::RadioKind::WiFi && !m_wifi) { m_wifi = r; m_wifiRev = r.StateChanged(winrt::auto_revoke, handler); }
            if (r.Kind() == wdr::RadioKind::Bluetooth && !m_bt) { m_bt = r; m_btRev = r.StateChanged(winrt::auto_revoke, handler); }
        }
    } catch (...) {}
    PostMessageW(notify, msg, 0, 0);
}

bool Radios::WifiOn() const { return m_wifi && m_wifi.State() == wdr::RadioState::On; }
bool Radios::BluetoothOn() const { return m_bt && m_bt.State() == wdr::RadioState::On; }

winrt::fire_and_forget Radios::Set(wdr::Radio radio, bool on) {
    if (!radio) co_return;
    if (DryRun()) { Log(std::wstring(L"DRYRUN radio ") + (radio.Kind() == wdr::RadioKind::WiFi ? L"wifi" : L"bluetooth") +
                        (on ? L" -> on" : L" -> off")); co_return; }
    try {
        if (co_await wdr::Radio::RequestAccessAsync() != wdr::RadioAccessStatus::Allowed) co_return;
        co_await radio.SetStateAsync(on ? wdr::RadioState::On : wdr::RadioState::Off);
    } catch (...) {}
}

std::wstring ConnectedSsid() {
    try {
        auto profile = winrt::Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
        if (profile && profile.IsWlanConnectionProfile()) return profile.ProfileName().c_str();
    } catch (...) {}
    return {};
}

// ---- Now Playing ------------------------------------------------------------------------------
void Media::Start(HWND notify, UINT event, std::function<void()> changed) {
    Stop();
    m_notify = notify;
    m_event = event;
    m_changed = std::move(changed);
    Init(m_gen);
}

void Media::Stop() {
    m_gen++;
    m_mgrRev.revoke();
    m_pbRev.revoke();
    m_propRev.revoke();
    m_mgr = nullptr;
    m_sess = nullptr;
    m_state = {};
    m_changed = nullptr;
}

winrt::fire_and_forget Media::Init(unsigned gen) {
    try {
        auto mgr = co_await Manager::RequestAsync();
        if (gen != m_gen) co_return;
        m_mgr = mgr;
        HWND h = m_notify;
        UINT e = m_event;
        m_mgrRev = mgr.CurrentSessionChanged(winrt::auto_revoke, [h, e](auto&&, auto&&) { PostMessageW(h, e, 0, 0); });
        PullAsync();
    } catch (...) {}
}

winrt::fire_and_forget Media::PullAsync() {
    unsigned gen = m_gen;
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
            auto info = sess.GetPlaybackInfo();
            next.playing = info.PlaybackStatus() ==
                           winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            auto c = info.Controls();
            next.canPrev = c.IsPreviousEnabled();
            next.canNext = c.IsNextEnabled();
            auto props = co_await sess.TryGetMediaPropertiesAsync();
            if (gen != m_gen) co_return;
            next.title = props.Title().c_str();
            next.artist = props.Artist().c_str();
        }
        if (gen != m_gen) co_return;
        m_state = std::move(next);
        if (m_changed) m_changed();
    } catch (...) {}
}

void Media::PlayPause() {
    if (DryRun()) return Log(L"DRYRUN media play/pause");
    if (m_sess) m_sess.TryTogglePlayPauseAsync();
}
void Media::Next() {
    if (DryRun()) return Log(L"DRYRUN media next");
    if (m_sess) m_sess.TrySkipNextAsync();
}
void Media::Prev() {
    if (DryRun()) return Log(L"DRYRUN media previous");
    if (m_sess) m_sess.TrySkipPreviousAsync();
}

}  // namespace halo::svc
