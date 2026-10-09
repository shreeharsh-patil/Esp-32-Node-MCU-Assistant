#include "pocket_wifi.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi_default.h>
#include <cJSON.h>
#include <lwip/netdb.h>
#include <nvs.h>
#include <cstdio>
#include <cstring>
#include "application.h"
#include "pocket_backend.h"
#include "ssid_manager.h"
#include "wifi_manager.h"

namespace {
constexpr EventBits_t kIp = BIT0, kFail = BIT1, kScan = BIT2;
uint64_t Now() { return esp_timer_get_time() / 1000; }
constexpr char kPage[] = R"HTML(<!doctype html>
<html lang="en"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Shreeharsh Assistant setup</title>
<style>body{background:#000;color:#f6cba8;font:16px system-ui;max-width:420px;margin:40px auto;padding:20px}input,select,button{box-sizing:border-box;width:100%;padding:14px;margin:8px 0;border:1px solid #856957;border-radius:12px;background:#171717;color:#fff}button{background:#f6cba8;color:#000}small{color:#ccc}fieldset{border:0;padding:0}</style>
<h1>Connect your companion</h1><p>Choose a 2.4 GHz network.</p><label>Nearby networks<select id="net"><option value="">Choose or type below</option></select></label>
<form id="form"><label>Network name<input id="ssid" maxlength="32" required autocomplete="off"></label><label>Wi-Fi password<input id="password" type="password" maxlength="64" autocomplete="new-password"></label>
<fieldset id="backend" hidden><label>Your backend address<input id="backend_url" type="url" maxlength="192" placeholder="https://your-project.vercel.app" autocomplete="off"></label><label>Device token<input id="backend_token" type="password" minlength="32" maxlength="128" autocomplete="new-password"></label><small>Use the DEVICE_TOKEN from your server settings. Your AI provider API key stays on Vercel.</small></fieldset>
<button id="go">Save and connect</button></form><p id="status" role="status">Loading nearby networks...</p><small>Keep this page open. If your phone says the hotspot has no internet, choose Stay connected. Hidden networks can be typed manually.</small>
<script>
const $=id=>document.getElementById(id);
$('net').onchange=()=>{$('ssid').value=$('net').value};
async function scan(){try{const data=await(await fetch('/scan',{cache:'no-store'})).json();const current=$('net').value;$('net').replaceChildren(new Option('Choose or type below',''));for(const a of data)$('net').add(new Option(a.ssid+' ('+a.rssi+' dBm)',a.ssid));$('net').value=current}catch{}}
let finishing=false;
async function status(){try{const d=await(await fetch('/status',{cache:'no-store'})).json();$('status').textContent=d.status+(d.ip?' - IP '+d.ip:'');$('go').disabled=d.busy;$('backend').hidden=!d.custom_backend;$('backend_url').required=d.custom_backend;$('backend_token').required=d.custom_backend;if(d.backend_url&&!$('backend_url').value)$('backend_url').value=d.backend_url;if(d.status==='Connected'&&!finishing){finishing=true;$('status').textContent+=' - Saved. Starting your assistant.';setTimeout(()=>fetch('/finish',{method:'POST'}).catch(()=>{}),3000)}}catch{if(finishing)$('status').textContent='Wi-Fi saved. Rejoin your home network.'}}
$('form').onsubmit=async e=>{e.preventDefault();$('go').disabled=true;$('status').textContent='Connecting...';try{const r=await fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:$('ssid').value,password:$('password').value,backend_url:$('backend_url').value,backend_token:$('backend_token').value})});const d=await r.json();$('password').value='';$('backend_token').value='';if(!r.ok)$('status').textContent=d.error}catch{$('status').textContent='Reconnect to the setup hotspot, then check again.'}setTimeout(status,1000)};
scan();status();setInterval(status,1500);setInterval(scan,10000);
</script></html>)HTML";
bool Rejected(int reason) {
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
}
bool Saved(const char* ssid, const char* password) {
    nvs_handle_t handle;
    if (nvs_open("wifi", NVS_READONLY, &handle) != ESP_OK)
        return false;
    bool found = false;
    for (unsigned i = 0; i < 10 && !found; ++i) {
        char key[16], value[65] = {};
        snprintf(key, sizeof(key), i ? "ssid%u" : "ssid", i);
        size_t size = sizeof(value);
        if (nvs_get_str(handle, key, value, &size) != ESP_OK || strcmp(value, ssid) != 0)
            continue;
        snprintf(key, sizeof(key), i ? "password%u" : "password", i);
        size = sizeof(value);
        found = nvs_get_str(handle, key, value, &size) == ESP_OK && strcmp(value, password) == 0;
        memset(value, 0, sizeof(value));
    }
    nvs_close(handle);
    return found;
}
}  // namespace

bool PocketWifi::Start(PocketDisplay* display, NetworkEventCallback callback) {
    display_ = display;
    callback_ = std::move(callback);
    WifiManagerConfig config;
    config.ssid_prefix = pocket::kWifiSetupSsid;
    config.language = "en-US";
    config.station_scan_min_interval_seconds = 10;
    config.station_scan_max_interval_seconds = 300;
    config.station_failure_retry_cnt = 2;
    config.station_hostname = "pocket-ai";
    if (!WifiManager::GetInstance().Initialize(config))
        return false;
    requests_ = xQueueCreate(2, sizeof(Request));
    events_ = xEventGroupCreate();
    if (!requests_ || !events_)
        return false;
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, Event, this,
                                            &wifi_handler_) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, Event, this,
                                            &ip_handler_) != ESP_OK)
        return false;
    WifiManager::GetInstance().SetEventCallback([this](WifiEvent event, const std::string& data) {
        if (event == WifiEvent::Connected) {
            Notify(NetworkEvent::Connected, data);
        } else if (event == WifiEvent::Disconnected) {
            Notify(NetworkEvent::Disconnected, data);
        } else if (event == WifiEvent::Scanning) {
            Notify(NetworkEvent::Scanning);
        } else if (event == WifiEvent::Connecting) {
            Notify(NetworkEvent::Connecting, data);
        }
    });
    return xTaskCreate([](void* ctx) { static_cast<PocketWifi*>(ctx)->Worker(); }, "pocket_wifi",
                       6144, this, 2, nullptr) == pdPASS;
}

void PocketWifi::Notify(NetworkEvent event, const std::string& data) {
    // Never mutate Application state from Wi-Fi/esp_timer callbacks.
    Application::GetInstance().Schedule([this, event, data]() {
        if (display_) {
            if (event == NetworkEvent::Connected) {
                display_->WifiFeedback("Connected",
                                       WifiManager::GetInstance().GetIpAddress().c_str());
            } else if (event == NetworkEvent::Disconnected) {
                display_->WifiFeedback("Reconnecting");
            } else if (event == NetworkEvent::Connecting || event == NetworkEvent::Scanning) {
                display_->WifiFeedback("Connecting to Wi-Fi");
            }
        }
        if (callback_)
            callback_(event, data);
    });
}

void PocketWifi::Setup(bool reset) {
    if (!requests_)
        return;
    Request request{reset ? Command::Reset : Command::Setup};
    if (xQueueSend(requests_, &request, 0) != pdTRUE)
        ESP_LOGW("PocketWifi", "Setup request queue busy; try again");
}

void PocketWifi::SetStatus(const char* status) {
    std::lock_guard<std::mutex> lock(mutex_);
    snprintf(status_, sizeof(status_), "%s", status);
}

bool PocketWifi::OpenPortal() {
    if (portal_.load())
        return true;
    WifiManager::GetInstance().StopStation();
    // Both interfaces are required: AP for the phone, STA for DHCP verification.
    portal_sta_ = esp_netif_create_default_wifi_sta();
    portal_ap_ = esp_netif_create_default_wifi_ap();
    if (!portal_sta_ || !portal_ap_)
        return false;
    uint8_t secret[8];
    esp_fill_random(secret, sizeof(secret));
    snprintf(setup_ssid_, sizeof(setup_ssid_), "%s", pocket::kWifiSetupSsid);
    for (unsigned i = 0; i < sizeof(secret); ++i)
        snprintf(setup_key_ + 2 * i, 3, "%02X", secret[i]);
    wifi_config_t ap{};
    memcpy(ap.ap.ssid, setup_ssid_, strlen(setup_ssid_));
    ap.ap.ssid_len = strlen(setup_ssid_);
    memcpy(ap.ap.password, setup_key_, 16);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;
    ap.ap.pmf_cfg.capable = true;
    // Wi-Fi driver storage is volatile; only verified credentials go to NVS.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    portal_.store(true);
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK || esp_wifi_start() != ESP_OK)
        return false;
    esp_wifi_set_ps(WIFI_PS_NONE);  // Keep provisioning HTTP responsive.
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 6;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 3;
    config.send_wait_timeout = 3;
    if (httpd_start(&server_, &config) != ESP_OK)
        return false;
    const struct {
        const char* path;
        httpd_method_t method;
    } routes[] = {{"/", HTTP_GET},
                  {"/scan", HTTP_GET},
                  {"/status", HTTP_GET},
                  {"/connect", HTTP_POST},
                  {"/finish", HTTP_POST}};
    for (const auto& route : routes) {
        httpd_uri_t uri{};
        uri.uri = route.path;
        uri.method = route.method;
        uri.handler = Http;
        uri.user_ctx = this;
        if (httpd_register_uri_handler(server_, &uri) != ESP_OK)
            return false;
    }
    SetStatus("Setting up Wi-Fi");
    next_scan_ = next_hint_ = 0;
    close_at_ = 0;
    completed_.store(false);
    Application::GetInstance().Schedule([this]() {
        auto& app = Application::GetInstance();
        app.ResetProtocol();
        const auto state = app.GetDeviceState();
        if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
            state == kDeviceStateConnecting || state == kDeviceStateNotifying) {
            app.AbortSpeaking(kAbortReasonNone);
            app.SetDeviceState(kDeviceStateIdle);
        }
        app.SetDeviceState(kDeviceStateWifiConfiguring);
        if (display_)
            display_->WifiSetup(setup_ssid_, setup_key_, false);
        if (callback_)
            callback_(NetworkEvent::WifiConfigModeEnter, "");
    });
    ESP_LOGI("PocketWifi", "Protected 2.4GHz setup AP %s; open http://192.168.4.1; key on LCD only",
             setup_ssid_);
    return true;
}

void PocketWifi::ClosePortal() {
    portal_.store(false);
    busy_.store(false);
    connect_queued_.store(false);
    completed_.store(false);
    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    esp_wifi_stop();
    if (portal_ap_)
        esp_netif_destroy_default_wifi(portal_ap_);
    if (portal_sta_)
        esp_netif_destroy_default_wifi(portal_sta_);
    portal_ap_ = portal_sta_ = nullptr;
    memset(setup_key_, 0, sizeof(setup_key_));
    memset(&pending_, 0, sizeof(pending_));
    close_at_ = 0;
}

void PocketWifi::Attempt(uint64_t now) {
    esp_wifi_scan_stop();
    esp_wifi_disconnect();
    // Drain the intentional disconnect before arming a new attempt. No waits
    // occur in the HTTP handler, application loop or audio tasks.
    vTaskDelay(pdMS_TO_TICKS(100));
    xEventGroupClearBits(events_, kIp | kFail);
    reason_.store(0);
    wifi_config_t station{};
    memcpy(station.sta.ssid, pending_.ssid, strlen(pending_.ssid));
    memcpy(station.sta.password, pending_.password, strlen(pending_.password));
    station.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    station.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    station.sta.failure_retry_cnt = 1;
    trial_.Start(now);
    if (esp_wifi_set_config(WIFI_IF_STA, &station) != ESP_OK || esp_wifi_connect() != ESP_OK)
        xEventGroupSetBits(events_, kFail);
    ESP_LOGI("PocketWifi", "Provisioning attempt %u/3; waiting for DHCP (credentials withheld)",
             trial_.attempts);
    SetStatus("Connecting to Wi-Fi");
    if (display_)
        display_->WifiFeedback("Connecting to Wi-Fi");
}

void PocketWifi::Worker() {
    auto& manager = WifiManager::GetInstance();
    if (SsidManager::GetInstance().GetSsidList().empty() ||
        (Board::GetInstance().UsesCustomBackend() && !pocket::BackendConfigured())) {
        if (!OpenPortal()) {
            ClosePortal();
            SetStatus("Setup failed; use serial diag");
            if (display_)
                display_->WifiFeedback("Setup failed. Restart to retry.");
        }
    } else {
        station_deadline_ = Now() + 60000;
        manager.StartStation();
    }
    while (true) {
        Request request{};
        if (xQueueReceive(requests_, &request, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (request.command == Command::Setup || request.command == Command::Reset) {
                if (portal_.load())
                    ClosePortal();
                manager.StopStation();
                if (request.command == Command::Reset)
                    SsidManager::GetInstance().Clear();  // Preserve UUID, volume and firmware.
                station_deadline_ = 0;
                if (!OpenPortal()) {
                    ClosePortal();
                    if (display_)
                        display_->WifiFeedback("Setup failed. Restart to retry.");
                }
            } else if (request.command == Command::Connect && portal_.load() && !busy_.load()) {
                connect_queued_.store(false);
                pending_ = request;
                trial_ = {};
                busy_.store(true);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ip_[0] = 0;
                }
                Attempt(Now());
            } else if (request.command == Command::Finish && close_at_) {
                close_at_ = Now() + 500;
            }
            memset(&request, 0, sizeof(request));
        }
        const uint64_t now = Now();
        if (!portal_.load()) {
            if (manager.IsConnected()) {
                station_deadline_ = 0;
                if (!had_connection_) {
                    had_connection_ = true;
                    Diagnostics();
                    // DNS is checked only on this worker, not in a Wi-Fi event callback.
                    addrinfo* address = nullptr;
                    std::string domain = "xiaozhi.me";
                    if (Board::GetInstance().UsesCustomBackend())
                        domain = Board::GetInstance().GetCustomBackendUrl().substr(8);
                    const int result = getaddrinfo(domain.c_str(), nullptr, nullptr, &address);
                    ESP_LOGI("PocketWifi", "DNS voice backend: %s (%d)",
                             result == 0 ? "OK" : "FAIL", result);
                    if (address)
                        freeaddrinfo(address);
                }
            } else {
                had_connection_ = false;
                if (station_deadline_ && now >= station_deadline_) {
                    station_deadline_ = 0;
                    ESP_LOGW("PocketWifi",
                             "Saved Wi-Fi unavailable after 60s; opening setup; credentials kept");
                    if (!OpenPortal()) {
                        ClosePortal();
                        if (display_)
                            display_->WifiFeedback("Setup failed. Restart to retry.");
                    }
                }
            }
            continue;
        }
        const EventBits_t bits =
            xEventGroupWaitBits(events_, kIp | kFail | kScan, pdTRUE, pdFALSE, 0);
        if (busy_.load() && (bits & kIp)) {
            wifi_ap_record_t ap{};
            esp_wifi_sta_get_ap_info(&ap);
            SsidManager::GetInstance().AddSsid(pending_.ssid, pending_.password, ap.primary);
            bool saved = Saved(pending_.ssid, pending_.password);
            if (saved && Board::GetInstance().UsesCustomBackend())
                saved = pocket::SaveBackend(pending_.backend_url, pending_.backend_token);
            memset(pending_.password, 0, sizeof(pending_.password));
            memset(pending_.backend_token, 0, sizeof(pending_.backend_token));
            busy_.store(false);
            if (!saved) {
                SetStatus("Storage failed. Restart and retry");
                if (display_)
                    display_->WifiFeedback("Wi-Fi storage failed");
                continue;
            }
            completed_.store(true);
            SetStatus("Connected");
            close_at_ = now + 15000;  // Automatic return even if the browser closes.
            char ip[16];
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snprintf(ip, sizeof(ip), "%s", ip_);
            }
            ESP_LOGI("PocketWifi", "Provisioning DHCP verified, IP=%s; saved to NVS", ip);
            if (display_)
                display_->WifiFeedback("Connected", ip);
        } else if (busy_.load() && trial_.deadline && ((bits & kFail) || now >= trial_.deadline)) {
            const int reason = reason_.load();
            esp_wifi_disconnect();
            const bool rejected = Rejected(reason);
            if (!trial_.Fail(now, rejected)) {
                busy_.store(false);
                memset(pending_.password, 0, sizeof(pending_.password));
                memset(pending_.backend_token, 0, sizeof(pending_.backend_token));
                const char* message = rejected ? "Connection failed: check password"
                                      : reason == WIFI_REASON_NO_AP_FOUND
                                          ? "Connection failed: network unavailable"
                                          : "Connection failed: no IP. Try again";
                SetStatus(message);
                if (display_)
                    display_->WifiFeedback("Connection failed", "Check 2.4GHz and password");
            } else {
                SetStatus("Retrying connection");
            }
            ESP_LOGW("PocketWifi", "Setup attempt failed, reason=%d; retries bounded", reason);
        }
        if (busy_.load() && trial_.retry_at && now >= trial_.retry_at)
            Attempt(now);
        if (close_at_ && now >= close_at_) {
            ClosePortal();
            if (display_)
                display_->WifiFeedback("Connecting to Wi-Fi");
            Notify(NetworkEvent::WifiConfigModeExit);
            station_deadline_ = now + 60000;
            manager.StartStation();
            continue;
        }
        if (!busy_.load() && !close_at_ && now >= next_scan_) {
            esp_wifi_scan_start(nullptr, false);
            next_scan_ = now + 10000;
        }
        if (!busy_.load() && !close_at_ && now >= next_hint_) {
            if (display_)
                display_->WifiSetup(setup_ssid_, setup_key_, show_key_);
            show_key_ = !show_key_;
            next_hint_ = now + 6000;
        }
    }
}

void PocketWifi::Event(void* ctx, esp_event_base_t base, int32_t id, void* data) {
    auto* self = static_cast<PocketWifi*>(ctx);
    if (!self->portal_.load())
        return;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const auto* event = static_cast<ip_event_got_ip_t*>(data);
        if (event->esp_netif != self->portal_sta_ || !event->ip_info.ip.addr)
            return;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            snprintf(self->ip_, sizeof(self->ip_), IPSTR, IP2STR(&event->ip_info.ip));
        }
        xEventGroupSetBits(self->events_, kIp);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        self->reason_.store(static_cast<wifi_event_sta_disconnected_t*>(data)->reason);
        xEventGroupSetBits(self->events_, kFail);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->ap_count_ = self->aps_.size();
        if (esp_wifi_scan_get_ap_records(&self->ap_count_, self->aps_.data()) != ESP_OK)
            self->ap_count_ = 0;
        xEventGroupSetBits(self->events_, kScan);
    }
}

esp_err_t PocketWifi::Http(httpd_req_t* req) {
    auto* self = static_cast<PocketWifi*>(req->user_ctx);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (strcmp(req->uri, "/") == 0) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, kPage, sizeof(kPage) - 1);
    }
    httpd_resp_set_type(req, "application/json");
    if (strcmp(req->uri, "/connect") == 0) {
        bool expected = false;
        if (self->busy_.load() || self->completed_.load() || !self->portal_.load() ||
            !self->connect_queued_.compare_exchange_strong(expected, true)) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "{\"error\":\"A connection is already in progress\"}");
        }
        char body[769] = {};
        if (req->content_len <= 0 || req->content_len > 768) {
            self->connect_queued_.store(false);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "{\"error\":\"Invalid request size\"}");
        }
        int received = 0;
        while (received < req->content_len) {
            const int n = httpd_req_recv(req, body + received, req->content_len - received);
            if (n <= 0) {
                self->connect_queued_.store(false);
                memset(body, 0, sizeof(body));
                return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request incomplete");
            }
            received += n;
        }
        cJSON* json = cJSON_Parse(body);
        Request request{Command::Connect};
        auto* ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
        auto* password = cJSON_GetObjectItemCaseSensitive(json, "password");
        auto* backend_url = cJSON_GetObjectItemCaseSensitive(json, "backend_url");
        auto* backend_token = cJSON_GetObjectItemCaseSensitive(json, "backend_token");
        const bool custom = Board::GetInstance().UsesCustomBackend();
        const bool backend_valid =
            !custom || (cJSON_IsString(backend_url) && cJSON_IsString(backend_token) &&
                        pocket::ValidBackendUrl(backend_url->valuestring) &&
                        pocket::ValidDeviceToken(backend_token->valuestring));
        const bool valid = backend_valid && cJSON_IsString(ssid) && cJSON_IsString(password) &&
                           pocket::ValidWifiCredentials(ssid->valuestring, password->valuestring);
        if (valid) {
            snprintf(request.ssid, sizeof(request.ssid), "%s", ssid->valuestring);
            snprintf(request.password, sizeof(request.password), "%s", password->valuestring);
            if (custom) {
                snprintf(request.backend_url, sizeof(request.backend_url), "%s",
                         backend_url->valuestring);
                snprintf(request.backend_token, sizeof(request.backend_token), "%s",
                         backend_token->valuestring);
            }
        }
        if (password && password->valuestring)
            memset(password->valuestring, 0, strlen(password->valuestring));
        if (backend_token && backend_token->valuestring)
            memset(backend_token->valuestring, 0, strlen(backend_token->valuestring));
        cJSON_Delete(json);
        memset(body, 0, sizeof(body));
        const bool queued = valid && xQueueSend(self->requests_, &request, 0) == pdTRUE;
        memset(&request, 0, sizeof(request));
        if (!queued) {
            self->connect_queued_.store(false);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req,
                                      "{\"error\":\"Use an SSID of 1-32 bytes and a valid Wi-Fi "
                                      "password. Custom backend needs an HTTPS origin and a 32-128 "
                                      "character device token\"}");
        }
        httpd_resp_set_status(req, "202 Accepted");
        return httpd_resp_sendstr(req, "{\"accepted\":true}");
    }
    if (strcmp(req->uri, "/finish") == 0) {
        Request request{Command::Finish};
        xQueueSend(self->requests_, &request, 0);
        return httpd_resp_sendstr(req, "{}");
    }
    cJSON* json = strcmp(req->uri, "/scan") == 0 ? cJSON_CreateArray() : cJSON_CreateObject();
    if (!json)
        return ESP_ERR_NO_MEM;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        if (cJSON_IsArray(json)) {
            for (unsigned i = 0; i < self->ap_count_; ++i) {
                auto* item = cJSON_CreateObject();
                char ssid[33] = {};
                memcpy(ssid, self->aps_[i].ssid, 32);
                cJSON_AddStringToObject(item, "ssid", ssid);
                cJSON_AddNumberToObject(item, "rssi", self->aps_[i].rssi);
                cJSON_AddItemToArray(json, item);
            }
        } else {
            cJSON_AddStringToObject(json, "status", self->status_);
            cJSON_AddStringToObject(json, "ip", self->ip_);
            cJSON_AddBoolToObject(json, "busy", self->busy_.load() || self->connect_queued_.load());
            cJSON_AddBoolToObject(json, "custom_backend", Board::GetInstance().UsesCustomBackend());
            cJSON_AddStringToObject(json, "backend_url",
                                    Board::GetInstance().UsesCustomBackend()
                                        ? Board::GetInstance().GetCustomBackendUrl().c_str()
                                        : "");
        }
    }
    char* text = cJSON_PrintUnformatted(json);
    const esp_err_t result = text ? httpd_resp_sendstr(req, text) : ESP_ERR_NO_MEM;
    cJSON_free(text);
    cJSON_Delete(json);
    return result;
}

void PocketWifi::Diagnostics() {
    auto& manager = WifiManager::GetInstance();
    ESP_LOGI(
        "PocketWifi", "mode=%s DHCP=%s IP=%s RSSI=%d channel=%d last_reason=%d; passwords withheld",
        portal_.load() ? "setup" : "station", manager.IsConnected() ? "ready" : "pending",
        manager.GetIpAddress().c_str(), manager.GetRssi(), manager.GetChannel(), reason_.load());
}
