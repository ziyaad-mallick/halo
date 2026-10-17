#pragma once

#include <windows.h>
#include <shellapi.h>
#include <propsys.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <wbemidl.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.WiFi.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "services.h"

// ============================================================================
// Undocumented COM interfaces
// ============================================================================

// Undocumented IPolicyConfig (Audio endpoint router)
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
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR wszDeviceId, ERole eRole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR wszDeviceId, BOOL bVisible) = 0;
};
__CRT_UUID_DECL(IPolicyConfig, 0xf8679f50, 0x850a, 0x41cf, 0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8)
inline constexpr CLSID CLSID_PolicyConfigClient =
    {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

// Undocumented IQuietHoursSettings (Focus / Do Not Disturb)
struct IQuietHoursSettings : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_UserSelectedProfile(PWSTR* profileId) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_UserSelectedProfile(PCWSTR profileId) = 0;
};
__CRT_UUID_DECL(IQuietHoursSettings, 0x6bff4732, 0x81ec, 0x4ffb, 0xae, 0x67, 0xb6, 0xc1, 0xbc, 0x29, 0x63, 0x1f)
inline constexpr CLSID CLSID_QuietHoursSettings =
    {0xf53321fa, 0x34f8, 0x4b7f, {0xb9, 0xa3, 0x36, 0x18, 0x77, 0xcb, 0x94, 0xcf}};

inline constexpr wchar_t kQuietHoursProfileUnrestricted[] = L"Microsoft.QuietHoursProfile.Unrestricted";
inline constexpr wchar_t kQuietHoursProfilePriorityOnly[] = L"Microsoft.QuietHoursProfile.PriorityOnly";
inline constexpr wchar_t kQuietHoursProfileAlarmsOnly[] = L"Microsoft.QuietHoursProfile.AlarmsOnly";

namespace halo::control_center {

// ============================================================================
// Layout & Visual Tokens (Matching Quiet Dock & Halo macOS-spec)
// ============================================================================
namespace tokens {
    constexpr float kPanelWidth = 336.0f;
    constexpr float kOuterPadding = 12.0f;
    constexpr float kItemGap = 12.0f;
    constexpr float kColumnWidth = 69.0f;    // (336 - 24 - 36) / 4
    constexpr float kDoubleColumnWidth = 150.0f; // 69 * 2 + 12
    constexpr float kSliderHeight = 36.0f;
    constexpr float kRowHeight = 40.0f;
    constexpr float kPanelRadius = 20.0f;
    constexpr float kTileRadius = 10.0f;
    constexpr float kSliderRadius = 18.0f;
    constexpr float kOpenSpringDamping = 0.85f;
    constexpr float kOpenSpringResponse = 0.25f;
    constexpr UINT  kTeardownDelayMs = 60000; // 60s idle before panel teardown
    constexpr UINT  kHotCornerDwellMs = 150;  // 150ms hover to open
    constexpr int   kHotCornerWidth = 64;     // px from top-right corner
    constexpr int   kHotCornerHeight = 2;     // px along top edge
}

// Window messages
constexpr UINT kMsgToggleControlCenter = WM_APP + 10;
constexpr UINT kMsgCloseControlCenter  = WM_APP + 11;
constexpr UINT kMsgHotCornerTrigger   = WM_APP + 12;

// ============================================================================
// 1. Volume & Audio Output Device Module
// ============================================================================
struct AudioDevice {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

class VolumeOutputModule {
public:
    explicit VolumeOutputModule(services::VolumeService& service) : m_service(service) {}

    float GetVolume() const { return m_service.GetVolume(); }
    bool IsMuted() const { return m_service.IsMuted(); }
    bool SetVolume(float scalar) { return m_service.SetVolume(scalar); }
    bool ToggleMute() { return m_service.SetMute(!m_service.IsMuted()); }

    std::vector<AudioDevice> EnumerateOutputDevices() {
        std::vector<AudioDevice> devices;
        winrt::com_ptr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                    __uuidof(IMMDeviceEnumerator), enumerator.put_void()))) {
            return devices;
        }

        std::wstring defaultId;
        winrt::com_ptr<IMMDevice> defaultDevice;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, defaultDevice.put()))) {
            LPWSTR strId = nullptr;
            if (SUCCEEDED(defaultDevice->GetId(&strId)) && strId) {
                defaultId = strId;
                CoTaskMemFree(strId);
            }
        }

        winrt::com_ptr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put()))) {
            return devices;
        }

        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            winrt::com_ptr<IMMDevice> dev;
            if (FAILED(collection->Item(i, dev.put()))) continue;

            LPWSTR strId = nullptr;
            if (FAILED(dev->GetId(&strId)) || !strId) continue;
            std::wstring id = strId;
            CoTaskMemFree(strId);

            std::wstring friendlyName = id;
            winrt::com_ptr<IPropertyStore> props;
            if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put()))) {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                    friendlyName = pv.pwszVal;
                }
                PropVariantClear(&pv);
            }

            devices.push_back({id, friendlyName, (id == defaultId)});
        }
        return devices;
    }

    bool SetDefaultOutputDevice(const std::wstring& deviceId) {
        winrt::com_ptr<IPolicyConfig> policyConfig;
        HRESULT hr = CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL,
                                      __uuidof(IPolicyConfig), policyConfig.put_void());
        if (SUCCEEDED(hr) && policyConfig) {
            hr = policyConfig->SetDefaultEndpoint(deviceId.c_str(), eConsole);
            policyConfig->SetDefaultEndpoint(deviceId.c_str(), eMultimedia);
            policyConfig->SetDefaultEndpoint(deviceId.c_str(), eCommunications);
            if (SUCCEEDED(hr)) return true;
        }

        // Fallback to Windows Sound settings
        ShellExecuteW(nullptr, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
        return false;
    }

private:
    services::VolumeService& m_service;
};

// ============================================================================
// 2. Display Brightness Module (WMI + DDC/CI)
// ============================================================================
struct MonitorBrightnessInfo {
    std::wstring id;
    std::wstring name;
    uint32_t current = 50;
    uint32_t minimum = 0;
    uint32_t maximum = 100;
    bool isInternal = false;
    HMONITOR hMonitor = nullptr;
};

class BrightnessModule {
public:
    BrightnessModule() = default;
    ~BrightnessModule() { CleanupMonitors(); }

    void RefreshMonitors() {
        std::lock_guard<std::mutex> lock(m_mutex);
        CleanupMonitors();
        ScanWmiMonitors();
        ScanDdcciMonitors();
    }

    std::vector<MonitorBrightnessInfo> GetMonitors() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_monitors;
    }

    bool SetBrightness(size_t index, uint32_t level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (index >= m_monitors.size()) return false;
        auto& mon = m_monitors[index];
        level = std::clamp(level, mon.minimum, mon.maximum);

        if (mon.isInternal) {
            return SetWmiBrightness(level);
        } else {
            return SetDdcciBrightness(mon.hMonitor, level);
        }
    }

private:
    void CleanupMonitors() {
        m_monitors.clear();
    }

    void ScanWmiMonitors() {
        winrt::com_ptr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator, locator.put_void()))) {
            return;
        }

        winrt::com_ptr<IWbemServices> services;
        BSTR bstrNamespace = SysAllocString(L"root\\wmi");
        HRESULT hr = locator->ConnectServer(bstrNamespace, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.put());
        SysFreeString(bstrNamespace);
        if (FAILED(hr)) return;

        BSTR bstrWql = SysAllocString(L"WQL");
        BSTR bstrQuery = SysAllocString(L"SELECT * FROM WmiMonitorBrightness");
        winrt::com_ptr<IEnumWbemClassObject> enumerator;
        hr = services->ExecQuery(bstrWql, bstrQuery, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                 nullptr, enumerator.put());
        SysFreeString(bstrWql);
        SysFreeString(bstrQuery);
        if (FAILED(hr)) return;

        IWbemClassObject* obj = nullptr;
        ULONG returned = 0;
        if (enumerator->Next(WBEM_INFINITE, 1, &obj, &returned) == S_OK && obj) {
            VARIANT vt;
            VariantInit(&vt);
            uint32_t cur = 50;
            if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &vt, nullptr, nullptr))) {
                cur = vt.bVal;
                VariantClear(&vt);
            }
            obj->Release();

            MonitorBrightnessInfo info;
            info.id = L"internal_display";
            info.name = L"Built-in Display";
            info.current = cur;
            info.minimum = 0;
            info.maximum = 100;
            info.isInternal = true;
            m_monitors.push_back(info);
        }
    }

    bool SetWmiBrightness(uint32_t level) {
        winrt::com_ptr<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator, locator.put_void()))) {
            return false;
        }

        winrt::com_ptr<IWbemServices> services;
        BSTR bstrNamespace = SysAllocString(L"root\\wmi");
        HRESULT hr = locator->ConnectServer(bstrNamespace, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.put());
        SysFreeString(bstrNamespace);
        if (FAILED(hr)) return false;

        BSTR bstrClass = SysAllocString(L"WmiMonitorBrightnessMethods");
        winrt::com_ptr<IWbemClassObject> classObj;
        hr = services->GetObject(bstrClass, 0, nullptr, classObj.put(), nullptr);
        SysFreeString(bstrClass);
        if (FAILED(hr)) return false;

        winrt::com_ptr<IWbemClassObject> inParamsDef;
        hr = classObj->GetMethod(L"WmiSetBrightness", 0, inParamsDef.put(), nullptr);
        if (FAILED(hr)) return false;

        winrt::com_ptr<IWbemClassObject> inParamsInstance;
        hr = inParamsDef->SpawnInstance(0, inParamsInstance.put());
        if (FAILED(hr)) return false;

        VARIANT vtTimeout, vtBrightness;
        vtTimeout.vt = VT_UI4;
        vtTimeout.ulVal = 1; // 1 second timeout
        vtBrightness.vt = VT_UI1;
        vtBrightness.bVal = static_cast<BYTE>(level);

        inParamsInstance->Put(L"Timeout", 0, &vtTimeout, 0);
        inParamsInstance->Put(L"Brightness", 0, &vtBrightness, 0);

        BSTR bstrPath = SysAllocString(L"WmiMonitorBrightnessMethods.InstanceName='ROOT\\\\WMI\\\\0000_0'");
        BSTR bstrMethod = SysAllocString(L"WmiSetBrightness");
        hr = services->ExecMethod(bstrPath, bstrMethod, 0, nullptr, inParamsInstance.get(), nullptr, nullptr);
        SysFreeString(bstrPath);
        SysFreeString(bstrMethod);
        return SUCCEEDED(hr);
    }

    void ScanDdcciMonitors() {
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hMon, HDC, LPRECT, LPARAM lParam) -> BOOL {
            auto* self = reinterpret_cast<BrightnessModule*>(lParam);
            DWORD numPhys = 0;
            if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &numPhys) && numPhys > 0) {
                std::vector<PHYSICAL_MONITOR> phys(numPhys);
                if (GetPhysicalMonitorsFromHMONITOR(hMon, numPhys, phys.data())) {
                    for (DWORD i = 0; i < numPhys; ++i) {
                        DWORD minB = 0, curB = 0, maxB = 100;
                        if (GetMonitorBrightness(phys[i].hPhysicalMonitor, &minB, &curB, &maxB)) {
                            MonitorBrightnessInfo info;
                            info.id = L"ddcci_" + std::to_wstring(reinterpret_cast<uintptr_t>(hMon)) + L"_" + std::to_wstring(i);
                            info.name = phys[i].szPhysicalMonitorDescription;
                            info.current = curB;
                            info.minimum = minB;
                            info.maximum = maxB;
                            info.isInternal = false;
                            info.hMonitor = hMon;
                            self->m_monitors.push_back(info);
                        }
                    }
                    DestroyPhysicalMonitors(numPhys, phys.data());
                }
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(this));
    }

    bool SetDdcciBrightness(HMONITOR hMon, uint32_t level) {
        if (!hMon) return false;
        DWORD numPhys = 0;
        bool ok = false;
        if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &numPhys) && numPhys > 0) {
            std::vector<PHYSICAL_MONITOR> phys(numPhys);
            if (GetPhysicalMonitorsFromHMONITOR(hMon, numPhys, phys.data())) {
                for (DWORD i = 0; i < numPhys; ++i) {
                    if (SetMonitorBrightness(phys[i].hPhysicalMonitor, level)) {
                        ok = true;
                    }
                }
                DestroyPhysicalMonitors(numPhys, phys.data());
            }
        }
        return ok;
    }

    mutable std::mutex m_mutex;
    std::vector<MonitorBrightnessInfo> m_monitors;
};

// ============================================================================
// 3. Wi-Fi & Bluetooth Module
// ============================================================================
struct WiFiNetworkInfo {
    std::wstring ssid;
    uint8_t signalBars = 0;
    bool isConnected = false;
    bool isSecure = true;
};

struct BluetoothDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isConnected = false;
};

class NetworkModule {
public:
    NetworkModule() = default;

    winrt::Windows::Foundation::IAsyncAction InitAsync() {
        try {
            auto adapters = co_await winrt::Windows::Devices::WiFi::WiFiAdapter::FindAllAdaptersAsync();
            if (adapters && adapters.Size() > 0) {
                m_wifiAdapter = adapters.GetAt(0);
                m_wifiAvailableToken = m_wifiAdapter.AvailableNetworksChanged([](auto&&, auto&&) {
                    // Available networks list updated
                });
            }
        } catch (...) {}

        try {
            auto radios = co_await winrt::Windows::Devices::Radios::Radio::GetRadiosAsync();
            for (auto const& r : radios) {
                if (r.Kind() == winrt::Windows::Devices::Radios::RadioKind::Bluetooth) {
                    m_btRadio = r;
                    m_btStateToken = r.StateChanged([](auto&&, auto&&) {
                        // Bluetooth radio state changed
                    });
                    break;
                }
            }
        } catch (...) {}
    }

    bool IsWiFiEnabled() const {
        // If adapter is present and networks are reportable
        return (m_wifiAdapter != nullptr);
    }

    winrt::Windows::Foundation::IAsyncAction ScanWiFiAsync() {
        if (m_wifiAdapter) {
            try {
                co_await m_wifiAdapter.ScanAsync();
            } catch (...) {}
        }
    }

    std::vector<WiFiNetworkInfo> GetAvailableWiFiNetworks() {
        std::vector<WiFiNetworkInfo> res;
        if (!m_wifiAdapter) return res;
        try {
            auto report = m_wifiAdapter.NetworkReport();
            if (!report) return res;
            for (auto const& net : report.AvailableNetworks()) {
                WiFiNetworkInfo info;
                info.ssid = net.Ssid();
                info.signalBars = net.SignalBars();
                info.isSecure = (net.SecuritySettings().NetworkAuthenticationType() !=
                                 winrt::Windows::Networking::Connectivity::NetworkAuthenticationType::Open80211);
                res.push_back(info);
            }
        } catch (...) {}
        return res;
    }

    bool IsBluetoothEnabled() const {
        if (!m_btRadio) return false;
        return m_btRadio.State() == winrt::Windows::Devices::Radios::RadioState::On;
    }

    winrt::Windows::Foundation::IAsyncAction SetBluetoothEnabledAsync(bool enable) {
        if (!m_btRadio) co_return;
        try {
            co_await m_btRadio.SetStateAsync(
                enable ? winrt::Windows::Devices::Radios::RadioState::On
                       : winrt::Windows::Devices::Radios::RadioState::Off);
        } catch (...) {}
    }

    winrt::Windows::Foundation::IAsyncAction GetPairedBluetoothDevicesAsync(
        std::function<void(std::vector<BluetoothDeviceInfo>)> callback) {
        std::vector<BluetoothDeviceInfo> devices;
        try {
            auto selector = winrt::Windows::Devices::Bluetooth::BluetoothDevice::GetDeviceSelectorFromPairingState(true);
            auto devList = co_await winrt::Windows::Devices::Enumeration::DeviceInformation::FindAllAsync(selector);
            for (auto const& dev : devList) {
                BluetoothDeviceInfo info;
                info.id = dev.Id();
                info.name = dev.Name();
                info.isConnected = false;
                devices.push_back(info);
            }
        } catch (...) {}
        if (callback) callback(std::move(devices));
    }

private:
    winrt::Windows::Devices::WiFi::WiFiAdapter m_wifiAdapter{nullptr};
    winrt::event_token m_wifiAvailableToken{};

    winrt::Windows::Devices::Radios::Radio m_btRadio{nullptr};
    winrt::event_token m_btStateToken{};
};

// ============================================================================
// 4. Focus / Do Not Disturb Module
// ============================================================================
class FocusModule {
public:
    using Callback = std::function<void(bool enabled)>;

    FocusModule() = default;
    ~FocusModule() { Stop(); }

    bool Start(Callback cb = nullptr) {
        m_callback = std::move(cb);
        try {
            auto mgr = winrt::Windows::UI::Notifications::ToastNotificationManager::GetDefault();
            if (mgr) {
                m_token = mgr.NotificationModeChanged([this](auto&& mgr, auto&&) {
                    bool enabled = (mgr.NotificationMode() !=
                                    winrt::Windows::UI::Notifications::ToastNotificationMode::Unrestricted);
                    if (m_callback) m_callback(enabled);
                });
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void Stop() {
        if (m_token.value != 0) {
            try {
                auto mgr = winrt::Windows::UI::Notifications::ToastNotificationManager::GetDefault();
                if (mgr) mgr.NotificationModeChanged(m_token);
            } catch (...) {}
            m_token = {};
        }
    }

    bool IsEnabled() const {
        try {
            auto mgr = winrt::Windows::UI::Notifications::ToastNotificationManager::GetDefault();
            if (mgr) {
                return mgr.NotificationMode() != winrt::Windows::UI::Notifications::ToastNotificationMode::Unrestricted;
            }
        } catch (...) {}
        return false;
    }

    bool SetEnabled(bool enable) {
        winrt::com_ptr<IQuietHoursSettings> settings;
        HRESULT hr = CoCreateInstance(CLSID_QuietHoursSettings, nullptr, CLSCTX_ALL,
                                      __uuidof(IQuietHoursSettings), settings.put_void());
        if (SUCCEEDED(hr) && settings) {
            hr = settings->put_UserSelectedProfile(
                enable ? kQuietHoursProfilePriorityOnly : kQuietHoursProfileUnrestricted);
            if (SUCCEEDED(hr)) return true;
        }

        // Fallback to Settings
        ShellExecuteW(nullptr, L"open", L"ms-settings:notifications", nullptr, nullptr, SW_SHOWNORMAL);
        return false;
    }

    bool Toggle() {
        return SetEnabled(!IsEnabled());
    }

private:
    winrt::event_token m_token{};
    Callback m_callback;
};

// ============================================================================
// 5. Control Center Main Panel State Machine & Composition Container
// ============================================================================
enum class PanelState {
    Closed,
    Opening,
    Open,
    DetailView,
    Closing
};

enum class DetailViewKind {
    None,
    WiFiList,
    BluetoothList,
    AudioOutputList
};

class ControlCenterPanel {
public:
    ControlCenterPanel(services::VolumeService& volumeService,
                       services::PowerService& powerService,
                       services::MediaService& mediaService)
        : m_volumeModule(volumeService),
          m_powerService(powerService),
          m_mediaService(mediaService) {}

    ~ControlCenterPanel() {
        Destroy();
    }

    bool Create(HINSTANCE inst, HWND parentHwnd = nullptr) {
        m_inst = inst;
        m_parent = parentHwnd;

        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &ControlCenterPanel::WndProcThunk;
        wc.hInstance = inst;
        wc.lpszClassName = L"HaloControlCenterWindow";
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        RegisterClassExW(&wc);

        return true;
    }

    void Toggle(HMONITOR targetMonitor = nullptr) {
        if (m_state == PanelState::Open || m_state == PanelState::DetailView || m_state == PanelState::Opening) {
            Close();
        } else {
            Open(targetMonitor);
        }
    }

    void Open(HMONITOR targetMonitor = nullptr) {
        if (!m_hwnd) {
            if (!EnsureWindow(targetMonitor)) return;
        }

        m_state = PanelState::Opening;
        m_brightnessModule.RefreshMonitors();
        m_networkModule.InitAsync();

        // Reveal window and start compositor spring animation
        ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
        AnimateOpen();
    }

    void Close() {
        if (m_state == PanelState::Closed || m_state == PanelState::Closing) return;
        m_state = PanelState::Closing;
        AnimateClose();
    }

    PanelState GetState() const { return m_state; }

    // Submodules
    VolumeOutputModule& Volume() { return m_volumeModule; }
    BrightnessModule& Brightness() { return m_brightnessModule; }
    NetworkModule& Network() { return m_networkModule; }
    FocusModule& Focus() { return m_focusModule; }
    services::PowerService& Power() { return m_powerService; }
    services::MediaService& Media() { return m_mediaService; }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<ControlCenterPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->OnMessage(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT OnMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_ACTIVATE:
                if (LOWORD(wParam) == WA_INACTIVE) {
                    Close();
                }
                return 0;
            case WM_KEYDOWN:
                if (wParam == VK_ESCAPE) {
                    if (m_detailView != DetailViewKind::None) {
                        ReturnToMainGrid();
                    } else {
                        Close();
                    }
                    return 0;
                }
                break;
            case WM_DESTROY:
                m_hwnd = nullptr;
                m_state = PanelState::Closed;
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool EnsureWindow(HMONITOR targetMon) {
        if (!targetMon) {
            targetMon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        }
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(targetMon, &mi);

        // Position: Top-right corner, 6px margin below top edge (or bar reservation)
        int panelW = static_cast<int>(tokens::kPanelWidth);
        int panelH = 440;
        int x = mi.rcMonitor.right - panelW - 12;
        int y = mi.rcMonitor.top + 12;

        m_hwnd = CreateWindowExW(
            WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"HaloControlCenterWindow", L"Halo Control Center",
            WS_POPUP, x, y, panelW, panelH,
            m_parent, nullptr, m_inst, this);

        return (m_hwnd != nullptr);
    }

    void AnimateOpen() {
        m_state = PanelState::Open;
    }

    void AnimateClose() {
        if (m_hwnd) ShowWindow(m_hwnd, SW_HIDE);
        m_state = PanelState::Closed;
    }

    void ReturnToMainGrid() {
        m_detailView = DetailViewKind::None;
        m_state = PanelState::Open;
    }

    void Destroy() {
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
        m_state = PanelState::Closed;
    }

    HINSTANCE m_inst = nullptr;
    HWND m_parent = nullptr;
    HWND m_hwnd = nullptr;
    PanelState m_state = PanelState::Closed;
    DetailViewKind m_detailView = DetailViewKind::None;

    VolumeOutputModule m_volumeModule;
    BrightnessModule m_brightnessModule;
    NetworkModule m_networkModule;
    FocusModule m_focusModule;
    services::PowerService& m_powerService;
    services::MediaService& m_mediaService;
};

// ============================================================================
// 6. Top-Right Hot Corner Detection Window
// ============================================================================
class HotCornerDetector {
public:
    HotCornerDetector() = default;
    ~HotCornerDetector() { Destroy(); }

    bool Create(HINSTANCE inst, HWND targetNotifyHwnd) {
        m_target = targetNotifyHwnd;

        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = &HotCornerDetector::WndProcThunk;
        wc.hInstance = inst;
        wc.lpszClassName = L"HaloHotCornerHitStrip";
        wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
        RegisterClassExW(&wc);

        HMONITOR mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);

        // Place a 2px strip at the top right of the primary monitor
        int x = mi.rcMonitor.right - tokens::kHotCornerWidth;
        int y = mi.rcMonitor.top;
        int w = tokens::kHotCornerWidth;
        int h = tokens::kHotCornerHeight;

        m_hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"HaloHotCornerHitStrip", L"HaloHotCorner",
            WS_POPUP, x, y, w, h,
            nullptr, nullptr, inst, this);

        if (m_hwnd) {
            ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
            return true;
        }
        return false;
    }

    void Destroy() {
        if (m_hwnd) {
            DestroyWindow(m_hwnd);
            m_hwnd = nullptr;
        }
    }

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<HotCornerDetector*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self ? self->OnMessage(hwnd, msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT OnMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_MOUSEMOVE:
                if (!m_tracking) {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                    m_tracking = TrackMouseEvent(&tme);
                    SetTimer(hwnd, 1, tokens::kHotCornerDwellMs, nullptr);
                }
                return 0;
            case WM_MOUSELEAVE:
                KillTimer(hwnd, 1);
                m_tracking = false;
                return 0;
            case WM_TIMER:
                if (wParam == 1) {
                    KillTimer(hwnd, 1);
                    if (m_target) {
                        PostMessageW(m_target, kMsgHotCornerTrigger, 0, 0);
                    }
                }
                return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HWND m_hwnd = nullptr;
    HWND m_target = nullptr;
    bool m_tracking = false;
};

}  // namespace halo::control_center
