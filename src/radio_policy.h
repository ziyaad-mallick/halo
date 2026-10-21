#pragma once

#include <windows.h>
#include <algorithm>

namespace halo::svc::radio_policy {

enum class DiscoveryPhase { Idle, Loading, Connecting, Ready, AccessDenied, Unavailable, Failed };
constexpr DWORD kRadioPowerStateInvalid = 0x80342002u;

inline DiscoveryPhase PhaseForWlanError(DWORD error) {
    if (error == ERROR_SUCCESS) return DiscoveryPhase::Ready;
    if (error == ERROR_ACCESS_DENIED) return DiscoveryPhase::AccessDenied;
    if (error == kRadioPowerStateInvalid || error == ERROR_SERVICE_NOT_ACTIVE ||
        error == ERROR_NOT_FOUND || error == ERROR_NO_MORE_ITEMS)
        return DiscoveryPhase::Unavailable;
    return DiscoveryPhase::Failed;
}

inline int SignalBars(ULONG quality) {
    quality = std::min<ULONG>(quality, 100);
    return quality == 0 ? 0 : static_cast<int>((quality + 24) / 25);
}

inline bool HasSavedProfile(wchar_t const* profileName) {
    return profileName && profileName[0] != L'\0';
}

}  // namespace halo::svc::radio_policy
