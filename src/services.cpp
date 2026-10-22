#include "services.h"
#include "shell_launch.h"

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
    m_notify = notify;
    m_msg = msg;
    Find(notify, msg, m_gen, *m_gen);
}

void Radios::Stop() {
    ++*m_gen;
    m_wifiRev.revoke();
    m_btRev.revoke();
    m_wifi = nullptr;
    m_bt = nullptr;
}

winrt::fire_and_forget Radios::Find(HWND notify, UINT msg, std::shared_ptr<unsigned> live, unsigned gen) {
    try {
        auto radios = co_await wdr::Radio::GetRadiosAsync();  // resumes on the UI thread
        if (gen != *live) co_return;
        for (auto const& r : radios) {
            auto handler = [notify, msg](auto&&, auto&&) { PostMessageW(notify, msg, 0, 0); };
            if (r.Kind() == wdr::RadioKind::WiFi && !m_wifi) { m_wifi = r; m_wifiRev = r.StateChanged(winrt::auto_revoke, handler); }
            if (r.Kind() == wdr::RadioKind::Bluetooth && !m_bt) { m_bt = r; m_btRev = r.StateChanged(winrt::auto_revoke, handler); }
        }
    } catch (...) {}
    PostMessageW(notify, msg, 0, 0);
}

// An unplugged USB adapter leaves a closed Radio whose State() throws.
static bool RadioIsOn(wdr::Radio const& r) {
    try { return r && r.State() == wdr::RadioState::On; } catch (...) { return false; }
}
bool Radios::WifiOn() const { return RadioIsOn(m_wifi); }
bool Radios::BluetoothOn() const { return RadioIsOn(m_bt); }

winrt::fire_and_forget Radios::Set(wdr::Radio radio, bool on, HWND notify, UINT msg) {
    if (!radio) co_return;
    if (DryRun()) { Log(std::wstring(L"DRYRUN radio ") + (radio.Kind() == wdr::RadioKind::WiFi ? L"wifi" : L"bluetooth") +
                        (on ? L" -> on" : L" -> off")); co_return; }
    try {
        if (co_await wdr::Radio::RequestAccessAsync() == wdr::RadioAccessStatus::Allowed)
            co_await radio.SetStateAsync(on ? wdr::RadioState::On : wdr::RadioState::Off);
    } catch (...) {}
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
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"), SMTO_ABORTIFHUNG, 200, &r);
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

void WifiConnectionReadback(unsigned gen, HWND notify, UINT msg, std::wstring profileName, GUID interfaceId) {
    try {
        for (int attempt = 0; attempt < 10 && gen == g_wifiGen.load(); ++attempt) {
            Sleep(1000);
            if (gen != g_wifiGen.load()) return;
            DWORD error = ERROR_SUCCESS;
            if (!WlanProfileConnected(interfaceId, profileName, error)) {
                if (error == ERROR_ACCESS_DENIED) {
                    {
                        std::lock_guard lock(g_wifiMutex);
                        if (gen == g_wifiGen.load()) g_wifiStatus = {DiscoveryPhase::AccessDenied, error};
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
                g_wifiStatus = {DiscoveryPhase::Ready, ERROR_SUCCESS};
            }
            if (notify) PostMessageW(notify, msg, 0, 0);
            return;
        }
        {
            std::lock_guard lock(g_wifiMutex);
            if (gen != g_wifiGen.load()) return;
            g_wifiStatus = {DiscoveryPhase::Failed, ERROR_TIMEOUT};
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
    } catch (...) {
        std::lock_guard lock(g_wifiMutex);
        if (gen == g_wifiGen.load()) g_wifiStatus = {DiscoveryPhase::Failed, ERROR_GEN_FAILURE};
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
        if (error == ERROR_SUCCESS) {
            std::unique_lock lock(signal->mutex);
            bool ended = signal->changed.wait_for(lock, std::chrono::seconds(4), [&] {
                return signal->complete || signal->cancelled || cancelled();
            });
            if (signal->cancelled || cancelled()) break;
            error = ended ? signal->result : ERROR_TIMEOUT;
        }
        if (error == ERROR_SUCCESS) {
            WlanMemory available;
            auto status = WlanGetAvailableNetworkList(wlan.value, &iface.InterfaceGuid, 0, nullptr,
                    reinterpret_cast<PWLAN_AVAILABLE_NETWORK_LIST*>(&available.value));
            if (status == ERROR_SUCCESS) {
                hadSuccess = true;
                auto networks = available.as<WLAN_AVAILABLE_NETWORK_LIST>();
                for (DWORD j = 0; networks && j < networks->dwNumberOfItems; ++j) {
                    auto const& item = networks->Network[j];
                    auto ssid = SsidText(item.dot11Ssid);
                    if (ssid.empty()) continue;
                    std::wstring profile = item.strProfileName;
                    nets.push_back({std::move(ssid), radio_policy::SignalBars(item.wlanSignalQuality),
                                    item.bSecurityEnabled != FALSE,
                                    (item.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0,
                                    std::move(profile), iface.InterfaceGuid});
                }
                error = ERROR_SUCCESS;
            } else error = status;
        }
        if (error != ERROR_SUCCESS && firstError == ERROR_SUCCESS) firstError = error;
    }

    registration.Reset();
    if (!cancelled()) {
        DWORD result = hadSuccess ? firstError : firstError == ERROR_SUCCESS ? ERROR_GEN_FAILURE : firstError;
        FinishWifi(gen, notify, msg, std::move(nets), result, hadAdapter, hadSuccess);
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

std::mutex g_btMutex;
std::vector<BtDevice> g_btDevices;
DiscoveryStatus g_btStatus;
unsigned g_btGen = 0;

struct BtEntry {
    std::wstring name;
    bool connected = false;
    winrt::Windows::Devices::Bluetooth::BluetoothDevice device{nullptr};
    winrt::Windows::Devices::Bluetooth::BluetoothDevice::ConnectionStatusChanged_revoker connectionRev;
};
struct BtSession {
    using Watcher = winrt::Windows::Devices::Enumeration::DeviceWatcher;
    std::atomic<bool> live{true};
    unsigned gen = 0;
    HWND notify{};
    UINT msg{};
    Watcher watcher{nullptr};
    Watcher::Added_revoker addedRev;
    Watcher::Updated_revoker updatedRev;
    Watcher::Removed_revoker removedRev;
    Watcher::EnumerationCompleted_revoker completedRev;
    Watcher::Stopped_revoker stoppedRev;
    std::unordered_map<std::wstring, unsigned> serials;
    std::unordered_map<std::wstring, BtEntry> entries;
    unsigned pending = 0;
    bool enumerated = false;
};
std::shared_ptr<BtSession> g_btSession;

void PostBt(std::shared_ptr<BtSession> const& session) {
    if (session && session->live.load() && session->notify) PostMessageW(session->notify, session->msg, 0, 0);
}
bool IsCurrentBt(std::shared_ptr<BtSession> const& session) {
    return session && session->live.load() && g_btSession == session && g_btGen == session->gen;
}
void SortBt() {
    std::stable_sort(g_btDevices.begin(), g_btDevices.end(), [](BtDevice const& a, BtDevice const& b) {
        if (a.connected != b.connected) return a.connected > b.connected;
        return a.name < b.name;
    });
}
void RebuildBt(std::shared_ptr<BtSession> const& session) {
    g_btDevices.clear();
    for (auto const& [_, entry] : session->entries)
        g_btDevices.push_back({entry.name, entry.connected});
    SortBt();
}
void BtConnectionChanged(std::weak_ptr<BtSession> weak, std::wstring id, unsigned serial,
                         winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus status) {
    auto session = weak.lock();
    if (!session) return;
    {
        std::lock_guard lock(g_btMutex);
        if (!IsCurrentBt(session)) return;
        auto current = session->serials.find(id);
        if (current == session->serials.end() || current->second != serial) return;
        auto entry = session->entries.find(id);
        if (entry == session->entries.end()) return;
        entry->second.connected = status == winrt::Windows::Devices::Bluetooth::BluetoothConnectionStatus::Connected;
        RebuildBt(session);
    }
    PostBt(session);
}

winrt::fire_and_forget ResolveBt(std::weak_ptr<BtSession> weak, std::wstring id, std::wstring name, unsigned serial) {
    namespace wdb = winrt::Windows::Devices::Bluetooth;
    auto session = weak.lock();
    if (!session || !session->live.load()) co_return;
    try {
        auto device = co_await wdb::BluetoothDevice::FromIdAsync(id);
        if (!device) throw winrt::hresult_error(E_FAIL, L"Bluetooth device lookup returned no device");
        if (name.empty()) name = device.Name().c_str();
        {
            std::lock_guard lock(g_btMutex);
            if (!IsCurrentBt(session)) co_return;
            auto current = session->serials.find(id);
            if (current != session->serials.end() && current->second == serial) {
                BtEntry entry;
                entry.name = name;
                entry.device = device;
                entry.connectionRev = device.ConnectionStatusChanged(winrt::auto_revoke,
                    [weak, id, serial](auto const& sender, auto const&) {
                        try { BtConnectionChanged(weak, id, serial, sender.ConnectionStatus()); } catch (...) {}
                    });
                entry.connected = device.ConnectionStatus() == wdb::BluetoothConnectionStatus::Connected;
                session->entries[id] = std::move(entry);
                RebuildBt(session);
            }
            if (session->pending) --session->pending;
            if (session->enumerated && !session->pending && g_btStatus.phase == DiscoveryPhase::Loading)
                g_btStatus = {DiscoveryPhase::Ready, ERROR_SUCCESS};
        }
        PostBt(session);
    } catch (winrt::hresult_error const& e) {
        {
            std::lock_guard lock(g_btMutex);
            if (!IsCurrentBt(session)) co_return;
            HRESULT hr = e.code();
            DWORD error = static_cast<DWORD>(hr);
            auto current = session->serials.find(id);
            if (current != session->serials.end() && current->second == serial && !name.empty()) {
                g_btStatus = {hr == E_ACCESSDENIED ? DiscoveryPhase::AccessDenied : DiscoveryPhase::Failed, error};
                session->entries[id].name = name;
                session->entries[id].connected = false;
                RebuildBt(session);
            }
            if (session->pending) --session->pending;
            if (session->enumerated && !session->pending && g_btStatus.phase == DiscoveryPhase::Loading)
                g_btStatus = {DiscoveryPhase::Ready, ERROR_SUCCESS};
        }
        PostBt(session);
    } catch (...) {
        {
            std::lock_guard lock(g_btMutex);
            if (!IsCurrentBt(session)) co_return;
            auto current = session->serials.find(id);
            if (current != session->serials.end() && current->second == serial)
                g_btStatus = {DiscoveryPhase::Failed, ERROR_GEN_FAILURE};
            if (session->pending) --session->pending;
            if (session->enumerated && !session->pending && g_btStatus.phase == DiscoveryPhase::Loading)
                g_btStatus = {DiscoveryPhase::Ready, ERROR_SUCCESS};
        }
        PostBt(session);
    }
}

void StopBtSession(std::shared_ptr<BtSession> session) {
    if (!session) return;
    session->live = false;
    session->addedRev.revoke();
    session->updatedRev.revoke();
    session->removedRev.revoke();
    session->completedRev.revoke();
    session->stoppedRev.revoke();
    std::unordered_map<std::wstring, BtEntry> entries;
    {
        std::lock_guard lock(g_btMutex);
        entries.swap(session->entries);
        session->serials.clear();
    }
    for (auto& [_, entry] : entries) entry.connectionRev.revoke();
    try { if (session->watcher) session->watcher.Stop(); } catch (...) {}
}

void BtResolveEvent(std::weak_ptr<BtSession> weak, std::wstring id, std::wstring name) {
    auto session = weak.lock();
    if (!session) return;
    unsigned serial;
    {
        std::lock_guard lock(g_btMutex);
        if (!IsCurrentBt(session)) return;
        serial = ++session->serials[id];
        ++session->pending;
        g_btStatus = {DiscoveryPhase::Loading, ERROR_SUCCESS};
    }
    ResolveBt(weak, std::move(id), std::move(name), serial);
}

}  // namespace

void WifiRefresh(HWND notify, UINT msg) {
    const unsigned gen = ++g_wifiGen;
    auto signal = std::make_shared<ScanSignal>();
    std::shared_ptr<ScanSignal> old;
    {
        std::lock_guard lock(g_wifiMutex);
        old = g_activeWifiScan.lock();
        g_activeWifiScan = signal;
        g_wifiNets.clear();
        g_wifiStatus = {DiscoveryPhase::Loading, ERROR_SUCCESS};
        g_wifiNotify = notify;
        g_wifiMsg = msg;
    }
    if (old) {
        std::lock_guard lock(old->mutex);
        old->cancelled = true;
        old->changed.notify_all();
    }
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
        g_wifiNets.clear();
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
    if (!radio_policy::HasSavedProfile(net.profileName.c_str())) {
        HINSTANCE result = ShellExecuteW(nullptr, L"open", L"ms-settings:network-wifi", nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) > 32) return true;
        {
            std::lock_guard lock(g_wifiMutex);
            if (generation == g_wifiGen.load()) g_wifiStatus = {DiscoveryPhase::Failed, ERROR_GEN_FAILURE};
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
        return false;
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
    if (error != ERROR_SUCCESS) {
        {
            std::lock_guard lock(g_wifiMutex);
            if (generation == g_wifiGen.load()) g_wifiStatus = {radio_policy::PhaseForWlanError(error), error};
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
        return false;
    }
    try {
        {
            std::lock_guard lock(g_wifiMutex);
            if (generation == g_wifiGen.load()) g_wifiStatus = {DiscoveryPhase::Connecting, ERROR_SUCCESS};
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
        std::thread(WifiConnectionReadback, generation, notify, msg, std::move(net.profileName), net.interfaceId).detach();
    } catch (...) {
        {
            std::lock_guard lock(g_wifiMutex);
            if (generation == g_wifiGen.load()) g_wifiStatus = {DiscoveryPhase::Failed, ERROR_NOT_ENOUGH_MEMORY};
        }
        if (notify) PostMessageW(notify, msg, 0, 0);
    }
    return true;
}

void BtRefresh(HWND notify, UINT msg) {
    auto session = std::make_shared<BtSession>();
    std::shared_ptr<BtSession> old;
    {
        std::lock_guard lock(g_btMutex);
        old = std::move(g_btSession);
        if (old) old->live = false;
        session->gen = ++g_btGen;
        session->notify = notify;
        session->msg = msg;
        g_btSession = session;
        g_btDevices.clear();
        g_btStatus = {DiscoveryPhase::Loading, ERROR_SUCCESS};
    }
    StopBtSession(std::move(old));
    try {
        namespace wdb = winrt::Windows::Devices::Bluetooth;
        namespace wde = winrt::Windows::Devices::Enumeration;
        auto selector = wdb::BluetoothDevice::GetDeviceSelectorFromPairingState(true);
        session->watcher = wde::DeviceInformation::CreateWatcher(selector);
        std::weak_ptr<BtSession> weak = session;
        session->addedRev = session->watcher.Added(winrt::auto_revoke, [weak](auto const&, auto const& info) {
            BtResolveEvent(weak, info.Id().c_str(), info.Name().c_str());
        });
        session->updatedRev = session->watcher.Updated(winrt::auto_revoke, [weak](auto const&, auto const& update) {
            BtResolveEvent(weak, update.Id().c_str(), {});
        });
        session->removedRev = session->watcher.Removed(winrt::auto_revoke, [weak](auto const&, auto const& update) {
            auto current = weak.lock();
            if (!current) return;
            {
                std::lock_guard lock(g_btMutex);
                if (!IsCurrentBt(current)) return;
                ++current->serials[update.Id().c_str()];
                current->entries.erase(update.Id().c_str());
                RebuildBt(current);
            }
            PostBt(current);
        });
        session->completedRev = session->watcher.EnumerationCompleted(winrt::auto_revoke, [weak](auto const&, auto const&) {
            auto current = weak.lock();
            if (!current) return;
            {
                std::lock_guard lock(g_btMutex);
                if (!IsCurrentBt(current)) return;
                current->enumerated = true;
                if (!current->pending && g_btStatus.phase == DiscoveryPhase::Loading)
                    g_btStatus = {DiscoveryPhase::Ready, ERROR_SUCCESS};
            }
            PostBt(current);
        });
        session->stoppedRev = session->watcher.Stopped(winrt::auto_revoke, [weak](auto const&, auto const&) {
            auto current = weak.lock();
            if (!current) return;
            {
                std::lock_guard lock(g_btMutex);
                if (!IsCurrentBt(current)) return;
                if (g_btStatus.phase == DiscoveryPhase::Loading) g_btStatus = {DiscoveryPhase::Failed, ERROR_GEN_FAILURE};
            }
            PostBt(current);
        });
        session->watcher.Start();
    } catch (winrt::hresult_error const& e) {
        std::lock_guard lock(g_btMutex);
        if (IsCurrentBt(session)) {
            HRESULT hr = e.code();
            g_btStatus = {hr == E_ACCESSDENIED ? DiscoveryPhase::AccessDenied : DiscoveryPhase::Failed, static_cast<DWORD>(hr)};
        }
        PostBt(session);
    } catch (...) {
        std::lock_guard lock(g_btMutex);
        if (IsCurrentBt(session)) g_btStatus = {DiscoveryPhase::Failed, ERROR_GEN_FAILURE};
        PostBt(session);
    }
}

void BtStop() {
    std::shared_ptr<BtSession> old;
    {
        std::lock_guard lock(g_btMutex);
        ++g_btGen;
        old = std::move(g_btSession);
        if (old) old->live = false;
        g_btDevices.clear();
        g_btStatus = {};
    }
    StopBtSession(std::move(old));
}

std::vector<BtDevice> BtDevices() {
    std::lock_guard lock(g_btMutex);
    return g_btDevices;
}
DiscoveryStatus BtStatus() {
    std::lock_guard lock(g_btMutex);
    return g_btStatus;
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
            auto info = sess.GetPlaybackInfo();
            next.playing = info.PlaybackStatus() ==
                           winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            auto c = info.Controls();
            next.canPrev = c.IsPreviousEnabled();
            next.canNext = c.IsNextEnabled();
            auto props = co_await sess.TryGetMediaPropertiesAsync();
            if (gen != *live || seq != *newest) co_return;
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
