#pragma once
#include <algorithm>
#include <cstdint>

namespace pocket {

// A failed voice connection must not cause a tight reconnect loop. Successful
// automatic listening restores the short delay for the next conversation.
class HandsFreePolicy {
public:
    bool ShouldStart(uint64_t now_ms, bool enabled, bool ready, bool listening) {
        if (!enabled || listening) {
            Reset(now_ms);
            return false;
        }
        if (!ready || now_ms < next_attempt_ms_)
            return false;
        next_attempt_ms_ = now_ms + retry_ms_;
        retry_ms_ = std::min<uint32_t>(retry_ms_ * 2, 60000);
        return true;
    }

    void Reset(uint64_t now_ms) {
        retry_ms_ = 2000;
        next_attempt_ms_ = now_ms + 1000;
    }

private:
    uint64_t next_attempt_ms_ = 0;
    uint32_t retry_ms_ = 2000;
};

}  // namespace pocket
