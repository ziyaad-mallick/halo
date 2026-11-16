// System services behind the Control Center: audio endpoints, battery and power mode,
// brightness (WMI), Wi-Fi / Bluetooth radios, Do Not Disturb, and Now Playing.
//
// UI clients own audio/radio/media objects. Discovery workers and watcher callbacks publish
// mutex-protected value snapshots and PostMessage their updates; callers copy those snapshots
// on the UI thread. Generation/session guards discard work after discovery is stopped.
//
// Responsiveness contract (fix/radios): nothing here blocks the calling thread on a radio, WLAN,
// Bluetooth or audio-endpoint call, so these functions are safe from the UI thread or a worker.
// The Wi-Fi, Bluetooth and output lists are cached in memory and survive WifiStop / BtStop, so a
// page can draw the last known rows at once (WifiNets / BtDevices / OutputDevicesCached) and then
// update; *Info() reports whether a refresh is running and how old the data is. Bluetooth and
// output updates are coalesced to at most one posted message per 100 ms.
#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <winrt/base.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Media.Control.h>
#include "radio_policy.h"
#include <atomic>
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

std::vector<Device> OutputDevices();  // blocking enumeration (~20 ms): UI code should prefer OutputDevicesCached()
// Accepted for processing: runs off the calling thread (IPolicyConfig for console, multimedia and communications
// roles; falls back to the Sound settings page). Returns true. The default-device notification (AudioEndpoint's
// kDeviceChanged) follows when Windows has switched; SetDefaultAudioOutput below reports the outcome directly.
bool SetDefaultOutput(std::wstring const& id);

// How fresh a cached list is. updatedTick is GetTickCount64() of the last applied update (0 = never).
struct ListInfo {
    bool hasData = false;
    bool refreshing = false;
    ULONGLONG updatedTick = 0;
    ULONGLONG AgeMs() const { return hasData ? GetTickCount64() - updatedTick : ~0ULL; }
};
// Output devices, cached: the last enumeration, instantly. OutputsRefresh() re-enumerates off-thread and posts
// `msg` (coalesced, at most one per 100 ms) when the list changed; it then keeps posting when endpoints come, go
// or change default, until OutputsStop(). Safe to call repeatedly: a running enumeration is not stacked.
std::vector<Device> OutputDevicesCached();
void OutputsRefresh(HWND notify, UINT msg);
void OutputsStop();  // stops posting; the cache stays
ListInfo OutputsInfo();

enum class AudioOpResult { Succeeded, FellBackToSettings, Failed };
// SetDefaultOutput on a worker thread. `done` (optional) runs on that worker; `msg` is also posted to `notify`.
void SetDefaultAudioOutput(std::wstring const& endpointId, HWND notify, UINT msg,
                           std::function<void(AudioOpResult)> done = {});
// ConnectBluetoothAudio is deliberately NOT implemented: the KS one-shot reconnect route is documented
// (KSPROPERTY_ONESHOT_RECONNECT) but its property-set GUID and the endpoint-to-IKsControl chain could not be
// confirmed from a primary source here. See the report for the design.

// ---- Power ----------------------------------------------------------------------------
struct Battery {
    int percent = 100;
    int remainingSeconds = -1;  // Windows estimate; -1 means unavailable.
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
// Probes run off the UI thread; writes share one worker per active burst and coalesce to the
// latest requested value. Failed writes remain retryable. Probe results post `msg` to `notify`.
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
    bool Loading() const { return m_loading; }
    bool Failed() const { return m_failed; }
    // While a change is pending these return the requested state, so a tile does not flicker back mid-change.
    bool WifiOn() const;
    bool BluetoothOn() const;
    // True from SetWifi/SetBluetooth until Windows has applied (or refused) the change. `msg` is posted when it ends.
    bool WifiPending() const { return m_wifiOp->want.load() >= 0; }
    bool BluetoothPending() const { return m_btOp->want.load() >= 0; }
    // Off-thread; the latest request wins if several arrive while one is running.
    void SetWifi(bool on) { Set(m_wifi, m_wifiOp, on, m_notify, m_msg); }
    void SetBluetooth(bool on) { Set(m_bt, m_btOp, on, m_notify, m_msg); }

private:
    struct Op {
        std::atomic<int> want{-1};  // -1 idle, 0/1 requested state
        std::atomic<bool> busy{false};
    };
    void Adopt(std::vector<winrt::Windows::Devices::Radios::Radio> const& radios, HWND notify, UINT msg);
    // The coroutines hold `live` (shared) and compare against it, never against a member, so one that resumes
    // after the owner is gone returns before it touches `this`.
    winrt::fire_and_forget Find(HWND notify, UINT msg, std::shared_ptr<unsigned> live, unsigned gen);
    static winrt::fire_and_forget Set(winrt::Windows::Devices::Radios::Radio radio, std::shared_ptr<Op> op, bool on, HWND notify, UINT msg);
    winrt::Windows::Devices::Radios::Radio m_wifi{nullptr}, m_bt{nullptr};
    winrt::Windows::Devices::Radios::Radio::StateChanged_revoker m_wifiRev, m_btRev;
    HWND m_notify{};
    UINT m_msg{};
    std::shared_ptr<Op> m_wifiOp = std::make_shared<Op>(), m_btOp = std::make_shared<Op>();
    std::function<void()> m_netUnsub;
    bool m_loading = false, m_failed = false;
    std::shared_ptr<unsigned> m_gen = std::make_shared<unsigned>(0);
};
std::wstring ConnectedSsid();  // empty when not on Wi-Fi. Blocking (NetworkInformation, ~20 ms): not for the UI thread
// The same, without blocking: the value of the last off-thread read. Radios::Start() keeps it fresh (it re-reads on
// every network status change and posts the radios message when the value changed).
std::wstring CachedSsid();
void SsidRefresh(HWND notify, UINT msg);

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
    std::wstring id;       // stable key: SSID + adapter. Key rows and presses by this, not by index.
    bool pending = false;  // a connect to this network is in flight
};
struct BtDevice {
    std::wstring name;
    bool connected = false;
    std::wstring id;     // stable key of the physical device: container id (Classic + LE entries merged), else address
    std::wstring aepId;  // the Classic entry's device id (else the LE one): what BluetoothDevice::FromIdAsync takes
    bool pending = false;
};
using DiscoveryPhase = radio_policy::DiscoveryPhase;
struct DiscoveryStatus {
    DiscoveryPhase phase = DiscoveryPhase::Idle;
    DWORD error = ERROR_SUCCESS;
};
// Returns at once. `msg` is posted immediately (so the page can re-read the cached rows), again when the quick
// read of the OS's cached scan results lands, and once more when the fresh scan finishes. A scan that is already
// running is joined, not restarted: pressing Refresh repeatedly does not stack scans. The cache survives WifiStop.
void WifiRefresh(HWND notify, UINT msg);
void WifiStop();  // cancels a running scan and stops posting; the cached rows stay
std::vector<WifiNet> WifiNets();  // rows: connected first, then by name (case-insensitive), then id
DiscoveryStatus WifiStatus();     // phase Loading == refreshing
ListInfo WifiInfo();
// Returns at once (the WLAN calls run on a worker). true means accepted; unknown profiles open Windows Wi-Fi
// Settings. The row is marked pending and status is Connecting until OS readback confirms the connection.
bool WifiConnect(WifiNet const& network);
// Bluetooth: one long-lived watcher pair (Classic + LE, paired, with connection state requested) that is started on
// the first BtRefresh and then kept running. BtRefresh returns at once, posts `msg` immediately (cached rows) and
// again as the list changes (coalesced to one per 100 ms); an explicit refresh while the watchers run reconciles
// against a fresh enumeration, bounded to a few seconds. BtStop only stops posting; the cache stays.
void BtRefresh(HWND notify, UINT msg);
void BtStop();
std::vector<BtDevice> BtDevices();  // connected first, then by name, then id; Classic + LE entries of one device merged
DiscoveryStatus BtStatus();         // phase Loading == refreshing
ListInfo BtInfo();

// ---- Now Playing (GSMTC) ------------------------------------------------------------------
struct MediaState {
    bool has = false, playing = false, canPrev = false, canNext = false;
    std::wstring title, artist, source;
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
