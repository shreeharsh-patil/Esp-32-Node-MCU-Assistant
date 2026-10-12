#include "pocket_clock.h"
#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <cstdlib>
#include <ctime>
#include "clock_state.h"
#include "settings.h"

namespace pocket {
std::string ClockTimezone() {
    Settings settings("pocket_clock");
    const auto name = settings.GetString("timezone", "Asia/Kolkata");
    return FindClockZone(name) ? name : "Asia/Kolkata";
}
void LoadClockTimezone() {
    const auto* zone = FindClockZone(ClockTimezone());
    setenv("TZ", zone->posix, 1);
    tzset();
}
bool SaveClockTimezone(const char* name) {
    if (!name || !FindClockZone(name))
        return false;
    {
        Settings settings("pocket_clock", true);
        settings.SetString("timezone", name);
    }
    if (ClockTimezone() != name)
        return false;
    LoadClockTimezone();
    return true;
}
void StartClockSync() {
    static bool initialized = false;  // Owned exclusively by PocketWifi::Worker.
    esp_err_t result;
    if (!initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");
        config.wait_for_sync = false;
        config.sync_cb = [](timeval*) { ESP_LOGI("PocketClock", "NTP synchronized UTC"); };
        result = esp_netif_sntp_init(&config);
        initialized = result == ESP_OK;
    } else {
        result = esp_netif_sntp_start();
    }
    ESP_LOGI("PocketClock", "Background NTP sync: %s", esp_err_to_name(result));
}
}  // namespace pocket
