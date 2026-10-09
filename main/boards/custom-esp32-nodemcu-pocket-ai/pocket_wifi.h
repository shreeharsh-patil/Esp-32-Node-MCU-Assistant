#pragma once
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include "board.h"
#include "pocket_display.h"
#include "pocket_wifi_policy.h"

// Board-owned secure provisioning. The upstream manager still owns normal STA
// scanning, saved credentials, reconnect backoff and power policy.
class PocketWifi {
public:
    bool Start(PocketDisplay* display, NetworkEventCallback callback);
    void Setup(bool reset = false);
    void Diagnostics();

private:
    enum class Command { Setup, Reset, Connect, Finish };
    struct Request {
        Command command;
        char ssid[33] = {}, password[65] = {};
        char backend_url[193] = {}, backend_token[129] = {};
    };
    PocketDisplay* display_ = nullptr;
    NetworkEventCallback callback_;
    QueueHandle_t requests_ = nullptr;
    EventGroupHandle_t events_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr, ip_handler_ = nullptr;
    esp_netif_t *portal_sta_ = nullptr, *portal_ap_ = nullptr;
    httpd_handle_t server_ = nullptr;
    std::atomic<bool> portal_{false}, busy_{false};
    std::atomic<bool> connect_queued_{false};
    std::atomic<bool> completed_{false};
    std::atomic<int> reason_{0};
    std::mutex mutex_;
    std::array<wifi_ap_record_t, 16> aps_{};
    uint16_t ap_count_ = 0;
    char status_[80] = "starting", ip_[16] = {}, setup_ssid_[32] = {}, setup_key_[17] = {};
    uint64_t station_deadline_ = 0, close_at_ = 0, next_scan_ = 0, next_hint_ = 0;
    bool show_key_ = true, had_connection_ = false;
    pocket::WifiTrial trial_;
    Request pending_{};

    void Worker();
    bool OpenPortal();
    void ClosePortal();
    void Attempt(uint64_t now);
    void SetStatus(const char* status);
    void Notify(NetworkEvent event, const std::string& data = "");
    static void Event(void* context, esp_event_base_t base, int32_t id, void* data);
    static esp_err_t Http(httpd_req_t* request);
};
