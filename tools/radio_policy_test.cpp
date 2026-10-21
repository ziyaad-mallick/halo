#include "../src/radio_policy.h"

#include <cassert>

int main() {
    using halo::svc::radio_policy::DiscoveryPhase;
    using halo::svc::radio_policy::HasSavedProfile;
    using halo::svc::radio_policy::PhaseForWlanError;
    using halo::svc::radio_policy::SignalBars;

    assert(PhaseForWlanError(ERROR_SUCCESS) == DiscoveryPhase::Ready);
    assert(PhaseForWlanError(ERROR_ACCESS_DENIED) == DiscoveryPhase::AccessDenied);
    assert(PhaseForWlanError(halo::svc::radio_policy::kRadioPowerStateInvalid) == DiscoveryPhase::Unavailable);
    assert(PhaseForWlanError(ERROR_SERVICE_NOT_ACTIVE) == DiscoveryPhase::Unavailable);
    assert(PhaseForWlanError(ERROR_GEN_FAILURE) == DiscoveryPhase::Failed);
    assert(SignalBars(0) == 0);
    assert(SignalBars(1) == 1 && SignalBars(25) == 1);
    assert(SignalBars(26) == 2 && SignalBars(50) == 2);
    assert(SignalBars(51) == 3 && SignalBars(75) == 3);
    assert(SignalBars(76) == 4 && SignalBars(100) == 4 && SignalBars(255) == 4);
    assert(HasSavedProfile(L"Home"));
    assert(!HasSavedProfile(L""));
    assert(!HasSavedProfile(nullptr));
}
