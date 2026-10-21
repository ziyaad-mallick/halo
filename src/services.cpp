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
#include <winrt/Windows.Storage.Streams.h>
#include <wincodec.h>
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
        auto profile = winrt::Windows::Networking::Connectivity::NetworkInformation::GetInternetConnectionProfile();
        if (profile && profile.IsWlanConnectionProfile()) return profile.ProfileName().c_str();
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

// ---- Wi-Fi networks and paired Bluetooth devices -----------------------------------------------------
namespace {
std::vector<WifiNet> g_nets;
std::vector<BtDevice> g_bt;
unsigned g_btGen = 0;

// Saved Wi-Fi profiles via the WLAN API. Scanning for new networks needs location permission since
// Windows 11 24H2 (WiFiAdapter answers "Access is denied" for an unpackaged app), so the list is the
// networks this PC already knows; joining one is WlanConnect on its profile.
GUID g_iface{};
bool OpenWlan(HANDLE& h) {
    DWORD ver = 0;
    return WlanOpenHandle(2, nullptr, &ver, &h) == ERROR_SUCCESS;
}
void BuildNets() {
    g_nets.clear();
    HANDLE h;
    if (!OpenWlan(h)) return;
    PWLAN_INTERFACE_INFO_LIST il = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &il) == ERROR_SUCCESS && il && il->dwNumberOfItems) {
        g_iface = il->InterfaceInfo[0].InterfaceGuid;
        PWLAN_PROFILE_INFO_LIST pl = nullptr;
        if (WlanGetProfileList(h, &g_iface, nullptr, &pl) == ERROR_SUCCESS && pl) {
            auto connected = ConnectedSsid();
            for (DWORD i = 0; i < pl->dwNumberOfItems && g_nets.size() < 8; i++) {
                std::wstring name = pl->ProfileInfo[i].strProfileName;
                g_nets.push_back({name, 0, true, name == connected});
            }
            WlanFreeMemory(pl);
        }
    }
    if (il) WlanFreeMemory(il);
    WlanCloseHandle(h, nullptr);
    std::stable_sort(g_nets.begin(), g_nets.end(), [](WifiNet const& a, WifiNet const& b) { return a.connected > b.connected; });
}

winrt::fire_and_forget BtTask(HWND notify, UINT msg, unsigned gen) {
    namespace wdb = winrt::Windows::Devices::Bluetooth;
    try {
        auto found = co_await winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(
            wdb::BluetoothDevice::GetDeviceSelectorFromPairingState(true));
        std::vector<BtDevice> out;
        for (auto const& d : found) {
            if (out.size() >= 6) break;
            BtDevice b{d.Name().c_str(), false};
            try {
                auto dev = co_await wdb::BluetoothDevice::FromIdAsync(d.Id());
                b.connected = dev && dev.ConnectionStatus() == wdb::BluetoothConnectionStatus::Connected;
            } catch (...) {}
            if (!b.name.empty()) out.push_back(std::move(b));
        }
        if (gen != g_btGen) co_return;
        std::sort(out.begin(), out.end(), [](BtDevice const& a, BtDevice const& b) { return a.connected > b.connected; });
        g_bt = std::move(out);
        PostMessageW(notify, msg, 0, 0);
    } catch (...) {}
}
}  // namespace

void WifiRefresh(HWND notify, UINT msg) {
    BuildNets();
    PostMessageW(notify, msg, 0, 0);
}
void WifiStop() { g_nets.clear(); }
std::vector<WifiNet> const& WifiNets() { return g_nets; }
void WifiConnect(std::wstring const& ssid) {
    if (DryRun()) return Log(L"DRYRUN join Wi-Fi " + ssid);
    HANDLE h;
    if (!OpenWlan(h)) return;
    WLAN_CONNECTION_PARAMETERS cp{};
    cp.wlanConnectionMode = wlan_connection_mode_profile;
    cp.strProfile = ssid.c_str();
    cp.dot11BssType = dot11_BSS_type_any;
    WlanConnect(h, &g_iface, &cp, nullptr);
    WlanCloseHandle(h, nullptr);
}
void BtRefresh(HWND notify, UINT msg) { BtTask(notify, msg, ++g_btGen); }
void BtStop() { g_btGen++; g_bt.clear(); }
std::vector<BtDevice> const& BtDevices() { return g_bt; }

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
            if (next.title == m_state.title && next.artist == m_state.artist) {
                next.art = m_state.art;  // same track: keep the decoded art
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
