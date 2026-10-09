#include <cassert>
#include <string>
#include "pocket_wifi_policy.h"
int main() {
    using pocket::ValidWifiCredentials;
    assert(std::string(pocket::kWifiSetupSsid) == "Shreeharsh Assistant");
    assert(ValidWifiCredentials("home", ""));
    assert(ValidWifiCredentials("2.4GHz network", "12345678"));
    assert(!ValidWifiCredentials("", "12345678"));
    assert(!ValidWifiCredentials(nullptr, "12345678"));
    assert(!ValidWifiCredentials("home", nullptr));
    assert(!ValidWifiCredentials(std::string(33, 's').c_str(), "12345678"));
    assert(ValidWifiCredentials(std::string(32, 's').c_str(), "12345678"));
    assert(!ValidWifiCredentials("home", "short"));
    assert(ValidWifiCredentials("home", std::string(63, 'z').c_str()));
    assert(ValidWifiCredentials("home", std::string(64, 'a').c_str()));
    assert(!ValidWifiCredentials("home", std::string(64, 'z').c_str()));
    assert(!ValidWifiCredentials("home", std::string(65, 'a').c_str()));
    // Deadlines and backoff still work at large monotonic timestamps.
    for (uint64_t now : {0ULL, 0xffffffffULL, 0x100000000ULL}) {
        pocket::WifiTrial trial;
        trial.Start(now);
        assert(trial.attempts == 1 && trial.deadline == now + 15000);
        assert(trial.Fail(now + 15000, false));
        assert(trial.deadline == 0 && trial.retry_at == now + 16000);
        trial.Start(trial.retry_at);
        assert(trial.attempts == 2 && trial.retry_at == 0);
        assert(trial.Fail(now + 31000, false));
        assert(trial.retry_at == now + 33000);
        trial.Start(trial.retry_at);
        assert(!trial.Fail(now + 48000, false));
        assert(trial.deadline == 0 && trial.retry_at == 0);
        pocket::WifiTrial rejected;
        rejected.Start(now);
        assert(!rejected.Fail(now + 1000, true));
        assert(rejected.deadline == 0 && rejected.retry_at == 0);
    }
}
