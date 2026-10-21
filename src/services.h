// System services behind the Control Center: audio endpoints, battery and power mode,
// brightness (WMI), Wi-Fi / Bluetooth radios, Do Not Disturb, and Now Playing.
//
// UI clients own audio/radio/media objects. Discovery workers and watcher callbacks publish
// mutex-protected value snapshots and PostMessage their updates; callers copy those snapshots
// on the UI thread. Generation/session guards discard work after discovery is stopped.
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <winrt/base.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Media.Control.h>
#include "radio_policy.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace halo::svc {

// HALO_TEST_CC_DRYRUN=1: actions are logged instead of performed.
bool DryRun();
void Log(std::wstring const& msg);

// ---- Audio ----------------------------------------------------------------------------
struct Device {
    std::wstring id, name;
    bool isDefault = false;
};

// The default endpoint of one direction (render = speakers, capture = microphone).
class AudioEndpoint {
public:
    // `msg` is posted to `notify` when someone else changes the volume or mute state, and with
    // wParam == kDeviceChanged when the default device of this direction changes. The owner
    // passes that wParam to OnMessage(), which re-binds to the new default device.
    static constexpr WPARAM kDeviceChanged = 1;
    bool Bind(EDataFlow flow, HWND notify, UINT msg);
    void OnMessage(WPARAM w);
    void Unbind();
    bool Available() const { return m_vol != nullptr; }
    float Volume() const;
    bool Muted() const;
    void SetVolume(float scalar);
    void SetMute(bool mute);

private:
    winrt::com_ptr<IAudioEndpointVolume> m_vol;
    winrt::com_ptr<IAudioEndpointVolumeCallback> m_cb;
    winrt::com_ptr<IMMDeviceEnumerator> m_en;
    winrt::com_ptr<IMMNotificationClient> m_nc;
    EDataFlow m_flow = eRender;
    HWND m_notify = nullptr;
    UINT m_msg = 0;
};

std::vector<Device> OutputDevices();
bool SetDefaultOutput(std::wstring const& id);  // false -> opened the Sound settings page instead

// ---- Power ----------------------------------------------------------------------------
struct Battery {
    int percent = 100;
    bool hasBattery = false, ac = true, charging = false;
};
Battery ReadBattery();

enum class PowerMode { Efficiency, Balanced, Performance, Max, Unknown };
PowerMode ReadPowerMode();
PowerMode NextPowerMode(PowerMode m);  // Efficiency -> Balanced -> Performance -> Efficiency
void SetPowerMode(PowerMode m);
wchar_t const* PowerModeName(PowerMode m);

// ---- Do Not Disturb ---------------------------------------------------------------------
// Win11's "Do not disturb" is HKCU\...\Notifications\Settings\NOC_GLOBAL_SETTING_TOASTS_ENABLED = 0.
bool ReadDnd();
void SetDnd(bool on);

// ---- Brightness (WMI, built-in panel; unsupported on desktops and most external monitors) ---
// Probe and Set run on short-lived worker threads (a WMI call takes 30-100 ms); the result
// lands in Supported()/Level() and `msg` is posted to `notify`.
namespace brightness {
void Probe(HWND notify, UINT msg);
bool Supported();
int Level();            // 0..100, -1 unknown
void Set(int percent);  // latest value wins when calls pile up
}

// ---- Wi-Fi and Bluetooth ------------------------------------------------------------------
class Radios {
public:
    void Start(HWND notify, UINT msg);  // finds the radios; posts `msg` once and on every state change
    void Stop();
    bool HasWifi() const { return m_wifi != nullptr; }
    bool HasBluetooth() const { return m_bt != nullptr; }
    bool WifiOn() const;
    bool BluetoothOn() const;
    void SetWifi(bool on) { Set(m_wifi, on, m_notify, m_msg); }
    void SetBluetooth(bool on) { Set(m_bt, on, m_notify, m_msg); }

private:
    // The coroutines hold `live` (shared) and compare against it, never against a member, so one that resumes
    // after the owner is gone returns before it touches `this`.
    winrt::fire_and_forget Find(HWND notify, UINT msg, std::shared_ptr<unsigned> live, unsigned gen);
    static winrt::fire_and_forget Set(winrt::Windows::Devices::Radios::Radio radio, bool on, HWND notify, UINT msg);
    winrt::Windows::Devices::Radios::Radio m_wifi{nullptr}, m_bt{nullptr};
    winrt::Windows::Devices::Radios::Radio::StateChanged_revoker m_wifiRev, m_btRev;
    HWND m_notify{};
    UINT m_msg{};
    std::shared_ptr<unsigned> m_gen = std::make_shared<unsigned>(0);
};
std::wstring ConnectedSsid();  // empty when not on Wi-Fi

// ---- Quick toggles ------------------------------------------------------------------------
bool ReadDarkMode();
void SetDarkMode(bool on);  // apps + system theme, then broadcasts the change
bool ReadNightLight();      // CloudStore blob; see services.cpp
void SetNightLight(bool on);
bool ReadEnergySaver();     // battery saver active (toggling is the Settings page: ms-settings:batterysaver)
bool HotspotAvailable();
bool ReadHotspot();
void SetHotspot(bool on);
void SendWinChord(WORD key, bool shift);  // Win+Shift+S, Win+K, Win+P: the shell owns these
void OpenSettings(wchar_t const* uri);

// ---- Wi-Fi networks and paired Bluetooth devices (inline lists) ----------------------------------
struct WifiNet {
    std::wstring ssid;
    int bars = 0;
    bool secure = true, connected = false;
    std::wstring profileName;
    GUID interfaceId{};
};
struct BtDevice {
    std::wstring name;
    bool connected = false;
};
using DiscoveryPhase = radio_policy::DiscoveryPhase;
struct DiscoveryStatus {
    DiscoveryPhase phase = DiscoveryPhase::Idle;
    DWORD error = ERROR_SUCCESS;
};
void WifiRefresh(HWND notify, UINT msg);  // async scan; one final status/list notification
void WifiStop();
std::vector<WifiNet> WifiNets();
DiscoveryStatus WifiStatus();
bool WifiConnect(WifiNet const& network);  // true means accepted; unknown profiles open Windows Wi-Fi Settings
void BtRefresh(HWND notify, UINT msg);
void BtStop();
std::vector<BtDevice> BtDevices();
DiscoveryStatus BtStatus();

// ---- Now Playing (GSMTC) ------------------------------------------------------------------
struct MediaState {
    bool has = false, playing = false, canPrev = false, canNext = false;
    std::wstring title, artist;
    int artSize = 0;  // album art is artSize x artSize premultiplied BGRA; 0 = none
    std::vector<uint32_t> art;
};

class Media {
public:
    // `event` is posted (foreign thread) whenever something changed; the owner then calls Pull().
    // `changed` runs on the UI thread when Pull() has a new state.
    void Start(HWND notify, UINT event, std::function<void()> changed);
    void Stop();
    void Pull() { PullAsync(); }
    MediaState const& State() const { return m_state; }
    void PlayPause();
    void Next();
    void Prev();

private:
    using Session = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
    using Manager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
    // As in Radios, the coroutines compare against shared counters, never members: a pull that resumes after
    // the owner is gone returns before touching `this`. m_seq drops a pull that a newer one has overtaken.
    winrt::fire_and_forget Init(std::shared_ptr<unsigned> live, unsigned gen);
    winrt::fire_and_forget PullAsync();
    HWND m_notify{};
    UINT m_event{};
    std::function<void()> m_changed;
    Manager m_mgr{nullptr};
    Session m_sess{nullptr};
    Manager::CurrentSessionChanged_revoker m_mgrRev;
    Session::PlaybackInfoChanged_revoker m_pbRev;
    Session::MediaPropertiesChanged_revoker m_propRev;
    MediaState m_state;
    std::shared_ptr<unsigned> m_gen = std::make_shared<unsigned>(0), m_seq = std::make_shared<unsigned>(0);
};

}  // namespace halo::svc
