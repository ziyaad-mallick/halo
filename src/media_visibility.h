#pragma once
#include <cstdint>

namespace halo {
// Event driven: only a playing -> paused transition starts the grace period.
struct MediaVisibility {
    uint64_t until = 0;
    bool wasPlaying = false;
    bool Update(bool has, bool playing, uint64_t now) {
        if (!has || playing) until = 0;
        else if (wasPlaying) until = now + 10000;
        wasPlaying = has && playing;
        return has && (playing || now < until);
    }
};
}
