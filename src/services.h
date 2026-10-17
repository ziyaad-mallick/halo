#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <powersetting.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#ifndef DEVICE_NOTIFY_CALLBACK
#define DEVICE_NOTIFY_CALLBACK 2
#endif

#ifndef _DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS
typedef ULONG (CALLBACK *PDEVICE_NOTIFY_CALLBACK_ROUTINE)(PVOID Context, ULONG Type, PVOID Setting);
typedef struct _DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS {
    PDEVICE_NOTIFY_CALLBACK_ROUTINE Callback;
    PVOID Context;
} DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS, *PDEVICE_NOTIFY_SUBSCRIBE_PARAMETERS;
#endif

namespace halo::services {

// Known power setting GUIDs
inline constexpr GUID kGuidBatteryPercentageRemaining =
    {0xa7ad8041, 0xb45a, 0x4cae, {0x87, 0xa3, 0xee, 0xcb, 0xb4, 0x68, 0xa9, 0xe1}};
inline constexpr GUID kGuidAcDcPowerSource =
    {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};

// ============================================================================
// 1. Audio Endpoint Volume & Device Listener
// ============================================================================
class VolumeService {
public:
    using Callback = std::function<void(float volume, bool muted)>;

    VolumeService() = default;
    ~VolumeService() { Stop(); }

    VolumeService(const VolumeService&) = delete;
    VolumeService& operator=(const VolumeService&) = delete;

    bool Start(Callback cb = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) return true;
        m_callback = std::move(cb);

        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                      __uuidof(IMMDeviceEnumerator), m_enumerator.put_void());
        if (FAILED(hr)) return false;

        m_notifyClient = winrt::make_self<NotificationClient>(this);
        m_enumerator->RegisterEndpointNotificationCallback(m_notifyClient.get());

        if (!BindCurrentEndpoint()) {
            // Audio service may be stopped or no render endpoint available
        }

        m_running = true;
        return true;
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) return;

        UnbindCurrentEndpoint();

        if (m_enumerator && m_notifyClient) {
            m_enumerator->UnregisterEndpointNotificationCallback(m_notifyClient.get());
        }
        m_notifyClient = nullptr;
        m_enumerator = nullptr;
        m_running = false;
    }

    float GetVolume() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_volume;
    }

    bool IsMuted() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_muted;
    }

    bool SetVolume(float scalar) {
        winrt::com_ptr<IAudioEndpointVolume> endpoint;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            endpoint = m_endpointVolume;
        }
        if (!endpoint) return false;
        scalar = std::clamp(scalar, 0.0f, 1.0f);
        return SUCCEEDED(endpoint->SetMasterVolumeLevelScalar(scalar, nullptr));
    }

    bool SetMute(bool mute) {
        winrt::com_ptr<IAudioEndpointVolume> endpoint;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            endpoint = m_endpointVolume;
        }
        if (!endpoint) return false;
        return SUCCEEDED(endpoint->SetMute(mute ? TRUE : FALSE, nullptr));
    }

    void SetCallback(Callback cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callback = std::move(cb);
    }

private:
    struct VolumeCallback : winrt::implements<VolumeCallback, IAudioEndpointVolumeCallback> {
        explicit VolumeCallback(VolumeService* owner) : m_owner(owner) {}

        STDMETHODIMP OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
            if (m_owner && data) {
                m_owner->HandleVolumeChange(data->fMasterVolume, data->bMuted != FALSE);
            }
            return S_OK;
        }

        void Detach() { m_owner = nullptr; }

    private:
        VolumeService* m_owner = nullptr;
    };

    struct NotificationClient : winrt::implements<NotificationClient, IMMNotificationClient> {
        explicit NotificationClient(VolumeService* owner) : m_owner(owner) {}

        STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR /*pwstrDefaultDeviceId*/) override {
            if (m_owner && flow == eRender && (role == eMultimedia || role == eConsole)) {
                m_owner->HandleDefaultDeviceChanged();
            }
            return S_OK;
        }

        STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
        STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return S_OK; }
        STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return S_OK; }
        STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

        void Detach() { m_owner = nullptr; }

    private:
        VolumeService* m_owner = nullptr;
    };

    bool BindCurrentEndpoint() {
        if (!m_enumerator) return false;

        winrt::com_ptr<IMMDevice> device;
        HRESULT hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, device.put());
        if (FAILED(hr)) return false;

        hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                              m_endpointVolume.put_void());
        if (FAILED(hr) || !m_endpointVolume) return false;

        float vol = 0.0f;
        BOOL mute = FALSE;
        m_endpointVolume->GetMasterVolumeLevelScalar(&vol);
        m_endpointVolume->GetMute(&mute);

        m_volume = vol;
        m_muted = (mute != FALSE);

        m_volumeCallback = winrt::make_self<VolumeCallback>(this);
        m_endpointVolume->RegisterControlChangeNotify(m_volumeCallback.get());
        return true;
    }

    void UnbindCurrentEndpoint() {
        if (m_endpointVolume && m_volumeCallback) {
            m_endpointVolume->UnregisterControlChangeNotify(m_volumeCallback.get());
            m_volumeCallback->Detach();
            m_volumeCallback = nullptr;
        }
        m_endpointVolume = nullptr;
    }

    void HandleDefaultDeviceChanged() {
        Callback cb;
        float v = 0.0f;
        bool m = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            UnbindCurrentEndpoint();
            BindCurrentEndpoint();
            v = m_volume;
            m = m_muted;
            cb = m_callback;
        }
        if (cb) cb(v, m);
    }

    void HandleVolumeChange(float vol, bool muted) {
        Callback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_volume = vol;
            m_muted = muted;
            cb = m_callback;
        }
        if (cb) cb(vol, muted);
    }

    mutable std::mutex m_mutex;
    bool m_running = false;
    float m_volume = 1.0f;
    bool m_muted = false;
    Callback m_callback;

    winrt::com_ptr<IMMDeviceEnumerator> m_enumerator;
    winrt::com_ptr<NotificationClient> m_notifyClient;
    winrt::com_ptr<IAudioEndpointVolume> m_endpointVolume;
    winrt::com_ptr<VolumeCallback> m_volumeCallback;
};

// ============================================================================
// 2. Battery & Power Listener
// ============================================================================
struct PowerState {
    uint8_t batteryPercent = 100;  // 0..100, 255 = unknown
    bool isAcOnline = true;
    bool isCharging = false;
    bool hasBattery = false;
    int effectivePowerMode = 2;    // EffectivePowerModeBalanced (2)
};

class PowerService {
public:
    using Callback = std::function<void(const PowerState& state)>;

    PowerService() = default;
    ~PowerService() { Stop(); }

    PowerService(const PowerService&) = delete;
    PowerService& operator=(const PowerService&) = delete;

    bool Start(Callback cb = nullptr) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) return true;
        m_callback = std::move(cb);

        UpdateSnapshotLocked();

        // 1. Battery percentage notification
        m_batteryParams.Callback = &PowerService::PowerSettingCallbackThunk;
        m_batteryParams.Context = this;
        DWORD err = PowerSettingRegisterNotification(&kGuidBatteryPercentageRemaining,
                                                     DEVICE_NOTIFY_CALLBACK,
                                                     (HANDLE)&m_batteryParams,
                                                     &m_batteryNotifyHandle);
        if (err != ERROR_SUCCESS) m_batteryNotifyHandle = nullptr;

        // 2. AC/DC power source notification
        m_acdcParams.Callback = &PowerService::PowerSettingCallbackThunk;
        m_acdcParams.Context = this;
        err = PowerSettingRegisterNotification(&kGuidAcDcPowerSource,
                                               DEVICE_NOTIFY_CALLBACK,
                                               (HANDLE)&m_acdcParams,
                                               &m_acdcNotifyHandle);
        if (err != ERROR_SUCCESS) m_acdcNotifyHandle = nullptr;

        // 3. Effective power mode notification (Windows 10 RS5+ / Windows 11)
        HRESULT hr = PowerRegisterForEffectivePowerModeNotifications(
            EFFECTIVE_POWER_MODE_V2,
            &PowerService::EffectiveModeCallbackThunk,
            this,
            &m_powerModeHandle);
        if (FAILED(hr)) m_powerModeHandle = nullptr;

        m_running = true;
        return true;
    }

    void Stop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) return;

        if (m_batteryNotifyHandle) {
            PowerSettingUnregisterNotification(m_batteryNotifyHandle);
            m_batteryNotifyHandle = nullptr;
        }
        if (m_acdcNotifyHandle) {
            PowerSettingUnregisterNotification(m_acdcNotifyHandle);
            m_acdcNotifyHandle = nullptr;
        }
        if (m_powerModeHandle) {
            PowerUnregisterFromEffectivePowerModeNotifications(m_powerModeHandle);
            m_powerModeHandle = nullptr;
        }

        m_running = false;
    }

    PowerState GetState() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    void SetCallback(Callback cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callback = std::move(cb);
    }

private:
    static ULONG CALLBACK PowerSettingCallbackThunk(PVOID context, ULONG type, PVOID setting) {
        auto* self = static_cast<PowerService*>(context);
        if (self) self->HandlePowerSettingChange(type, setting);
        return 0;
    }

    static VOID WINAPI EffectiveModeCallbackThunk(EFFECTIVE_POWER_MODE mode, VOID* context) {
        auto* self = static_cast<PowerService*>(context);
        if (self) self->HandlePowerModeChange(mode);
    }

    void UpdateSnapshotLocked() {
        SYSTEM_POWER_STATUS sps{};
        if (GetSystemPowerStatus(&sps)) {
            m_state.isAcOnline = (sps.ACLineStatus == 1);
            m_state.hasBattery = !(sps.BatteryFlag & 128); // 128 = No system battery
            m_state.isCharging = (sps.BatteryFlag & 8) != 0;
            m_state.batteryPercent = (sps.BatteryLifePercent <= 100) ? sps.BatteryLifePercent : 100;
        }
    }

    void HandlePowerSettingChange(ULONG /*type*/, PVOID /*setting*/) {
        Callback cb;
        PowerState s;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            UpdateSnapshotLocked();
            s = m_state;
            cb = m_callback;
        }
        if (cb) cb(s);
    }

    void HandlePowerModeChange(EFFECTIVE_POWER_MODE mode) {
        Callback cb;
        PowerState s;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state.effectivePowerMode = static_cast<int>(mode);
            s = m_state;
            cb = m_callback;
        }
        if (cb) cb(s);
    }

    mutable std::mutex m_mutex;
    bool m_running = false;
    PowerState m_state{};
    Callback m_callback;

    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS m_batteryParams{};
    DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS m_acdcParams{};
    HPOWERNOTIFY m_batteryNotifyHandle = nullptr;
    HPOWERNOTIFY m_acdcNotifyHandle = nullptr;
    VOID* m_powerModeHandle = nullptr;
};

// ============================================================================
// 3. Now Playing Listener (GSMTC Sessions)
// ============================================================================
struct MediaState {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring sourceAppId;
    bool isPlaying = false;
    bool canPlay = false;
    bool canPause = false;
    bool canSkipNext = false;
    bool canSkipPrevious = false;
    bool hasSession = false;
};

class MediaService {
public:
    using Callback = std::function<void(const MediaState& state)>;

    MediaService() = default;
    ~MediaService() { Stop(); }

    MediaService(const MediaService&) = delete;
    MediaService& operator=(const MediaService&) = delete;

    winrt::Windows::Foundation::IAsyncAction StartAsync(Callback cb = nullptr) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_running) co_return;
            m_callback = std::move(cb);
            m_running = true;
        }

        try {
            auto manager = co_await winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_running) co_return;
                m_manager = manager;
            }

            m_sessionChangedToken = manager.CurrentSessionChanged([this](auto&&, auto&&) {
                SyncCurrentSessionAsync();
            });

            m_sessionsChangedToken = manager.SessionsChanged([this](auto&&, auto&&) {
                SyncCurrentSessionAsync();
            });

            co_await SyncCurrentSessionAsync();
        } catch (...) {
            // GSMTC may throw on unsupported / unpackaged environments or during broker failure
        }
    }

    void Stop() {
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session{nullptr};
        winrt::event_token tokSessionChanged{}, tokSessionsChanged{}, tokPlayback{}, tokMedia{};

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) return;
            m_running = false;
            mgr = std::move(m_manager);
            session = std::move(m_currentSession);
            tokSessionChanged = m_sessionChangedToken;
            tokSessionsChanged = m_sessionsChangedToken;
            tokPlayback = m_playbackChangedToken;
            tokMedia = m_mediaChangedToken;
        }

        if (session) {
            try {
                session.PlaybackInfoChanged(tokPlayback);
                session.MediaPropertiesChanged(tokMedia);
            } catch (...) {}
        }
        if (mgr) {
            try {
                mgr.CurrentSessionChanged(tokSessionChanged);
                mgr.SessionsChanged(tokSessionsChanged);
            } catch (...) {}
        }
    }

    MediaState GetState() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_state;
    }

    void SetCallback(Callback cb) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callback = std::move(cb);
    }

    winrt::Windows::Foundation::IAsyncAction TryPlayPauseAsync() {
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session{nullptr};
        bool isPlaying = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            session = m_currentSession;
            isPlaying = m_state.isPlaying;
        }
        if (!session) co_return;
        try {
            if (isPlaying) {
                co_await session.TryPauseAsync();
            } else {
                co_await session.TryPlayAsync();
            }
        } catch (...) {}
    }

    winrt::Windows::Foundation::IAsyncAction TrySkipNextAsync() {
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session{nullptr};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            session = m_currentSession;
        }
        if (!session) co_return;
        try {
            co_await session.TrySkipNextAsync();
        } catch (...) {}
    }

    winrt::Windows::Foundation::IAsyncAction TrySkipPreviousAsync() {
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session{nullptr};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            session = m_currentSession;
        }
        if (!session) co_return;
        try {
            co_await session.TrySkipPreviousAsync();
        } catch (...) {}
    }

private:
    winrt::Windows::Foundation::IAsyncAction SyncCurrentSessionAsync() {
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) co_return;
            mgr = m_manager;
        }
        if (!mgr) co_return;

        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session{nullptr};
        try {
            session = mgr.GetCurrentSession();
        } catch (...) {
            session = nullptr;
        }

        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession oldSession{nullptr};
        winrt::event_token oldPlaybackTok{}, oldMediaTok{};
        bool sessionChanged = false;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (session != m_currentSession) {
                sessionChanged = true;
                oldSession = m_currentSession;
                oldPlaybackTok = m_playbackChangedToken;
                oldMediaTok = m_mediaChangedToken;

                m_currentSession = session;
            }
        }

        if (sessionChanged && oldSession) {
            try {
                oldSession.PlaybackInfoChanged(oldPlaybackTok);
                oldSession.MediaPropertiesChanged(oldMediaTok);
            } catch (...) {}
        }

        if (sessionChanged && session) {
            m_playbackChangedToken = session.PlaybackInfoChanged([this](auto&&, auto&&) {
                RefreshMediaStateAsync();
            });
            m_mediaChangedToken = session.MediaPropertiesChanged([this](auto&&, auto&&) {
                RefreshMediaStateAsync();
            });
        }

        co_await RefreshMediaStateAsync();
    }

    winrt::Windows::Foundation::IAsyncAction RefreshMediaStateAsync() {
        winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession session{nullptr};
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) co_return;
            session = m_currentSession;
        }

        MediaState next{};
        if (session) {
            next.hasSession = true;
            try {
                next.sourceAppId = session.SourceAppUserModelId();
            } catch (...) {}

            try {
                auto playback = session.GetPlaybackInfo();
                if (playback) {
                    using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
                    auto status = playback.PlaybackStatus();
                    next.isPlaying = (status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);

                    auto controls = playback.Controls();
                    if (controls) {
                        next.canPlay = controls.IsPlayEnabled();
                        next.canPause = controls.IsPauseEnabled();
                        next.canSkipNext = controls.IsNextEnabled();
                        next.canSkipPrevious = controls.IsPreviousEnabled();
                    }
                }
            } catch (...) {}

            try {
                auto mediaProps = co_await session.TryGetMediaPropertiesAsync();
                if (mediaProps) {
                    next.title = mediaProps.Title();
                    next.artist = mediaProps.Artist();
                    next.album = mediaProps.AlbumTitle();
                }
            } catch (...) {}
        }

        Callback cb;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = next;
            cb = m_callback;
        }
        if (cb) cb(next);
    }

    mutable std::mutex m_mutex;
    bool m_running = false;
    MediaState m_state{};
    Callback m_callback;

    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager m_manager{nullptr};
    winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession m_currentSession{nullptr};
    winrt::event_token m_sessionChangedToken{};
    winrt::event_token m_sessionsChangedToken{};
    winrt::event_token m_playbackChangedToken{};
    winrt::event_token m_mediaChangedToken{};
};

}  // namespace halo::services
