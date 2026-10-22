#include "../src/media_visibility.h"
#include <cassert>
#include <cstdio>
int main() {
    halo::MediaVisibility v;
    assert(!v.Update(true, false, 0)); // an already paused session doesn't appear at startup
    assert(v.Update(true, true, 100));
    assert(v.Update(true, false, 200));
    assert(v.until == 10200);
    assert(v.Update(true, false, 5000)); // metadata events cannot extend the timeout
    assert(v.until == 10200);
    assert(v.Update(true, false, 10199));
    assert(!v.Update(true, false, 10200));
    assert(v.Update(true, true, 11000));
    assert(v.Update(true, false, 12000));
    assert(v.Update(true, true, 13000)); // resuming cancels expiry
    assert(v.until == 0);
    assert(v.Update(true, false, 14000));
    assert(!v.Update(false, false, 14001)); // quitting the player hides immediately
    assert(v.until == 0);
    puts("media visibility: all checks passed");
}
