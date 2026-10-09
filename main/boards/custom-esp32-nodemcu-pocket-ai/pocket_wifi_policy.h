#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace pocket {
inline constexpr char kWifiSetupSsid[] = "Shreeharsh Assistant";
inline bool ValidWifiCredentials(const char* ssid, const char* password) {
    if (!ssid || !password)
        return false;
    const size_t s = std::strlen(ssid), p = std::strlen(password);
    if (!s || s > 32 || p > 64 || (p > 0 && p < 8))
        return false;
    if (p == 64) {
        for (size_t i = 0; i < p; ++i) {
            const char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
    }
    return true;
}
// Portal retries are bounded independently of the station's existing 10..300s
// scan backoff. A successful attempt requires DHCP, never association alone.
struct WifiTrial {
    unsigned attempts = 0;
    uint64_t deadline = 0, retry_at = 0;
    void Start(uint64_t now) {
        ++attempts;
        deadline = now + 15000;
        retry_at = 0;
    }
    bool Fail(uint64_t now, bool credentials_rejected) {
        deadline = 0;
        if (credentials_rejected || attempts >= 3)
            return false;
        retry_at = now + (1000ULL << (attempts - 1));
        return true;
    }
};
}  // namespace pocket
