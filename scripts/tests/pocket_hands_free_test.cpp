#include <cassert>
#include <cstdint>
#include "hands_free_policy.h"

int main() {
    pocket::HandsFreePolicy policy;
    const uint64_t now = UINT64_C(30000000000);
    // Setup, playback, offline and manual recording are all not ready.
    assert(!policy.ShouldStart(now, true, false, false));
    assert(policy.ShouldStart(now, true, true, false));
    assert(!policy.ShouldStart(now + 1999, true, true, false));
    assert(policy.ShouldStart(now + 2000, true, true, false));
    assert(!policy.ShouldStart(now + 5999, true, true, false));
    assert(policy.ShouldStart(now + 6000, true, true, false));
    // A successful listen must never be toggled off by the automatic controller.
    assert(!policy.ShouldStart(now + 6100, true, false, true));
    assert(!policy.ShouldStart(now + 7099, true, true, false));
    assert(policy.ShouldStart(now + 7100, true, true, false));
    // Pause remains off even with working network and Idle. Resume waits 1s.
    assert(!policy.ShouldStart(now + 8000, false, true, false));
    assert(!policy.ShouldStart(now + 8999, true, true, false));
    assert(policy.ShouldStart(now + 9000, true, true, false));
    // Repeated service failures back off, capped at one attempt per minute.
    policy.Reset(now);
    uint64_t attempt = now + 1000;
    const uint32_t delays[] = {2000, 4000, 8000, 16000, 32000, 60000, 60000};
    for (uint32_t delay : delays) {
        assert(policy.ShouldStart(attempt, true, true, false));
        assert(!policy.ShouldStart(attempt + delay - 1, true, true, false));
        attempt += delay;
    }
    // A reconnect restores capture automatically without resetting the policy.
    assert(!policy.ShouldStart(attempt, true, false, false));
    assert(policy.ShouldStart(attempt, true, true, false));
}
