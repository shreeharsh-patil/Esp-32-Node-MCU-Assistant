#include <driver/spi_common.h>
#include <driver/uart.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include "application.h"
#include "config.h"
#include "device_state_machine.h"
#include "hands_free_policy.h"
#include "pocket_audio_codec.h"
#include "pocket_backend.h"
#include "pocket_display.h"
#include "pocket_wifi.h"
#include "system_info.h"
#include "wifi_board.h"
#include "wifi_manager.h"

static const char* TAG = "PocketBoard";

class PocketBoard : public WifiBoard {
public:
    PocketBoard() {
        spi_bus_config_t bus = {};
        bus.mosi_io_num = DISPLAY_MOSI;
        bus.miso_io_num = -1;
        bus.sclk_io_num = DISPLAY_SCK;
        bus.quadwp_io_num = bus.quadhd_io_num = -1;
        bus.max_transfer_sz = DISPLAY_WIDTH * 20 * 2;
        esp_err_t error = spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO);
        esp_lcd_panel_io_handle_t io = nullptr;
        esp_lcd_panel_handle_t panel = nullptr;
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS;
        io_config.dc_gpio_num = DISPLAY_DC;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 26 * 1000 * 1000;
        io_config.trans_queue_depth = 4;
        io_config.lcd_cmd_bits = io_config.lcd_param_bits = 8;
        if (error == ESP_OK)
            error = esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io);
        esp_lcd_panel_dev_config_t device = {};
        device.reset_gpio_num = DISPLAY_RST;
        device.rgb_ele_order = DISPLAY_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB;
        device.bits_per_pixel = 16;
        if (error == ESP_OK)
            error = esp_lcd_new_panel_st7789(io, &device, &panel);
        if (error == ESP_OK)
            error = esp_lcd_panel_reset(panel);
        if (error == ESP_OK)
            error = esp_lcd_panel_init(panel);
        if (error == ESP_OK)
            error = esp_lcd_panel_invert_color(panel, DISPLAY_INVERT);
        if (error == ESP_OK)
            error = esp_lcd_panel_swap_xy(panel, DISPLAY_SWAP_XY);
        if (error == ESP_OK)
            error = esp_lcd_panel_mirror(panel, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        if (error == ESP_OK)
            display_ = new PocketDisplay(io, panel, &audio_);
        else {
            ESP_LOGE(TAG, "TFT driver initialization FAIL: %s; serial interface remains available",
                     esp_err_to_name(error));
            if (panel)
                esp_lcd_panel_del(panel);
            if (io)
                esp_lcd_panel_io_del(io);
        }
        gpio_config_t button = {};
        button.pin_bit_mask = 1ULL << POCKET_PTT_GPIO;
        button.mode = GPIO_MODE_INPUT;
        button.pull_up_en = GPIO_PULLUP_ENABLE;
        gpio_config(&button);
        if (uart_driver_install(UART_NUM_0, 512, 0, 0, nullptr, 0) == ESP_OK) {
            xTaskCreate([](void* context) { static_cast<PocketBoard*>(context)->ControlTask(); },
                        "pocket_control", 4096, this, 1, nullptr);
        } else
            ESP_LOGE(TAG, "Serial command UART initialization failed");
    }

    AudioCodec* GetAudioCodec() override { return &audio_; }
    bool UsesCustomBackend() const override {
#if CONFIG_POCKET_CUSTOM_BACKEND
        return true;
#else
        return false;
#endif
    }
    std::string GetCustomBackendUrl() const override { return pocket::BackendUrl(); }
    std::string GetCustomBackendToken() const override { return pocket::BackendToken(); }
    Display* GetDisplay() override {
        return display_ ? static_cast<Display*>(display_) : &fallback_;
    }
    // Optional capabilities remain absent: no GPIO backlight, battery, camera, touch.
    void StartNetwork() override {
        uint32_t capacity = 0;
        if (esp_flash_get_size(nullptr, &capacity) != ESP_OK || capacity < 4 * 1024 * 1024) {
            GetDisplay()->SetStatus("Flash capacity error");
            GetDisplay()->SetChatMessage("system", "This release requires at least 4 MB flash.");
            ESP_LOGE(TAG, "Flash verification FAIL: detected %lu bytes", capacity);
            return;
        }
        if (!audio_.Ready()) {
            GetDisplay()->SetChatMessage("system", "I2S allocation failed. Use diag over serial.");
        }
#if CONFIG_POCKET_HARDWARE_DIAGNOSTICS
        Application::GetInstance().SetDeviceState(kDeviceStateWifiConfiguring);
        GetDisplay()->SetStatus("Hardware diagnostics");
        GetDisplay()->SetChatMessage("system",
                                     "Offline: colors / tone / testaudio / diag over serial");
        ESP_LOGI(TAG, "Diagnostic build: network and cloud disabled");
#else
        if (!network_.Start(display_, [this](NetworkEvent event, const std::string& data) {
                if (network_event_callback_)
                    network_event_callback_(event, data);
            })) {
            ESP_LOGE(TAG, "Wi-Fi initialization failed");
            if (display_)
                display_->WifiFeedback("Wi-Fi initialization failed");
        }
#endif
    }

private:
    PocketAudioCodec audio_;
    PocketDisplay* display_ = nullptr;
    NoDisplay fallback_;
#if !CONFIG_POCKET_HARDWARE_DIAGNOSTICS
    PocketWifi network_;
    pocket::HandsFreePolicy hands_free_policy_;
    std::atomic<bool> hands_free_enabled_{true};
#endif
    std::atomic<bool> pressed_{false};
    bool armed_ = false;
    int64_t start_time_ = 0;
    int64_t test_until_ = 0;
    uint32_t test_rx_before_ = 0;
    uint32_t test_tx_before_ = 0;
    uint32_t test_rx_errors_ = 0, test_tx_errors_ = 0, test_peak_ = 0;
    int64_t test_finish_ = 0;
    std::array<char, 96> command_{};
    size_t command_length_ = 0;

    void StartTalk() {
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        if (state != kDeviceStateIdle && state != kDeviceStateSpeaking &&
            state != kDeviceStateWifiConfiguring)
            return;
        if (!audio_.Ready()) {
            GetDisplay()->ShowNotification("I2S not ready");
            return;
        }
        if (display_)
            display_->Thinking(false);
        pressed_ = true;
        start_time_ = esp_timer_get_time();
        app.StartListening();
    }

    void StopTalk() {
        if (!pressed_)
            return;
        pressed_ = false;
        auto& app = Application::GetInstance();
        const bool conversation = app.GetDeviceState() != kDeviceStateAudioTesting &&
                                  app.GetDeviceState() != kDeviceStateWifiConfiguring;
        app.StopListening();
        if (conversation && display_) {
            display_->Thinking(true);
            display_->SetChatMessage("system", "Thinking...");
        }
    }

#if !CONFIG_POCKET_HARDWARE_DIAGNOSTICS
    void PollHandsFree(uint64_t now_ms) {
        auto& app = Application::GetInstance();
        const auto state = app.GetDeviceState();
        const bool enabled = hands_free_enabled_.load();
        if (!enabled && !pressed_ && state == kDeviceStateListening)
            app.StopListening();
        const bool ready = state == kDeviceStateIdle && !pressed_ && !test_until_ &&
                           audio_.Ready() && WifiManager::GetInstance().IsConnected() &&
                           app.GetAudioService().IsPlaybackIdle();
        if (!hands_free_policy_.ShouldStart(now_ms, enabled, ready, state == kDeviceStateListening))
            return;
        // Recheck on the application task. A reply, setup request or manual
        // recording may have started since the control task observed Idle.
        app.Schedule([this]() {
            auto& app = Application::GetInstance();
            if (hands_free_enabled_.load() && !pressed_ &&
                app.GetDeviceState() == kDeviceStateIdle &&
                WifiManager::GetInstance().IsConnected() &&
                app.GetAudioService().IsPlaybackIdle()) {
                ESP_LOGI(TAG, "Hands-free: start automatic listening; server detects speech end");
                app.ToggleChatState();
            }
        });
    }
#endif

    void Diagnostics() {
        esp_chip_info_t chip = {};
        esp_chip_info(&chip);
        uint32_t flash = 0;
        esp_flash_get_size(nullptr, &flash);
        const auto* app = esp_app_get_description();
        ESP_LOGI(TAG, "CHIP model=%d cores=%d flash=%lu version=%s SDK=%s reset=%d", chip.model,
                 chip.cores, flash, app->version, app->idf_ver, esp_reset_reason());
        ESP_LOGI(TAG, "HEAP internal_free=%u minimum=%u largest=%u dma_free=%u dma_largest=%u",
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 heap_caps_get_free_size(MALLOC_CAP_DMA),
                 heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        ESP_LOGI(TAG,
                 "AUDIO rms=%lu peak=%lu rx_frames=%lu tx_frames=%lu rx_errors=%lu tx_errors=%lu "
                 "overruns=%lu",
                 audio_.Rms(), audio_.Peak(), audio_.InputFrames(), audio_.OutputFrames(),
                 audio_.RxErrors(), audio_.TxErrors(), audio_.Overruns());
        ESP_LOGI(TAG,
                 "DMA sample storage=12288B; PCM scratch=2048B; TFT strip=11200B; PSRAM disabled");
        Application::GetInstance().GetAudioService().PrintDiagnostics();
        if (display_)
            display_->PrintUiDiagnostics();
#if !CONFIG_POCKET_HARDWARE_DIAGNOSTICS
        network_.Diagnostics();
        ESP_LOGI(TAG, "HANDS_FREE enabled=%d; server VAD, microphone pauses during replies",
                 hands_free_enabled_.load());
#endif
        ESP_LOGI(TAG, "STATE %s; physical tests require observed signals/sound; no synthetic PASS",
                 DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState()));
    }

    void HandleCommand(const char* command) {
        auto& app = Application::GetInstance();
        if (strcmp(command, "start") == 0)
            StartTalk();
        else if (strcmp(command, "stop") == 0) {
#if !CONFIG_POCKET_HARDWARE_DIAGNOSTICS
            hands_free_enabled_.store(false);
#endif
            StopTalk();
            app.StopListening();
        }
#if !CONFIG_POCKET_HARDWARE_DIAGNOSTICS
        else if (strcmp(command, "handsfree on") == 0 || strcmp(command, "handsfree off") == 0) {
            const bool enabled = strcmp(command, "handsfree on") == 0;
            hands_free_enabled_.store(enabled);
            hands_free_policy_.Reset(esp_timer_get_time() / 1000);
            if (!enabled && !pressed_)
                app.StopListening();
            GetDisplay()->ShowNotification(enabled ? "Hands-free on" : "Hands-free paused");
        }
#endif
        else if (strcmp(command, "cancel") == 0)
            app.Schedule([&app]() {
                app.AbortSpeaking(kAbortReasonNone);
                app.StopListening();
            });
        else if (strcmp(command, "diag") == 0 || strcmp(command, "info") == 0) {
            Diagnostics();
            char info[192];
            snprintf(info, sizeof(info),
                     "ESP32 / 4 MB / USB\n%s\nIDF %s\nHeap %u, min %u\nVolume %d (capped)",
                     esp_app_get_description()->version, esp_app_get_description()->idf_ver,
                     static_cast<unsigned>(esp_get_free_heap_size()),
                     static_cast<unsigned>(esp_get_minimum_free_heap_size()),
                     audio_.output_volume());
            GetDisplay()->SetChatMessage("system", info);
        } else if (strncmp(command, "face ", 5) == 0 && display_) {
            if (!display_->PreviewExpression(command + 5))
                ESP_LOGW(TAG, "Face preview requires idle/setup and a known expression");
        } else if (strcmp(command, "setup") == 0)
#if CONFIG_POCKET_HARDWARE_DIAGNOSTICS
            ESP_LOGI(TAG, "Offline diagnostics selected; use conversation firmware for Wi-Fi");
#else
            network_.Setup();
#endif
        else if (strcmp(command, "wifi-reset CONFIRM") == 0)
#if CONFIG_POCKET_HARDWARE_DIAGNOSTICS
            ESP_LOGI(TAG, "Use the conversation firmware to reset Wi-Fi only");
#else
            network_.Setup(true);
#endif
        else if (strcmp(command, "colors") == 0 && display_)
            display_->ColorTest(true);
        else if (strcmp(command, "home") == 0 && display_)
            display_->ColorTest(false);
        else if (strncmp(command, "volume ", 7) == 0) {
            char* end = nullptr;
            const long volume = std::strtol(command + 7, &end, 10);
            if (end == command + 7 || *end != '\0' || volume < 0 || volume > 100) {
                ESP_LOGW(TAG, "Use volume 0..100; the absolute output cap always applies");
                return;
            }
            audio_.SetOutputVolume(static_cast<int>(volume));
            GetDisplay()->ShowNotification("Volume set (output capped)");
        } else if (strcmp(command, "testaudio") == 0 || strcmp(command, "mic") == 0) {
            if (app.GetDeviceState() != kDeviceStateWifiConfiguring || test_until_ || pressed_) {
                ESP_LOGW(TAG, "testaudio requires Wi-Fi setup mode; enter setup first");
                return;
            }
            test_rx_before_ = audio_.InputFrames();
            test_tx_before_ = audio_.OutputFrames();
            test_rx_errors_ = audio_.RxErrors();
            test_tx_errors_ = audio_.TxErrors();
            StartTalk();
            test_until_ = esp_timer_get_time() + 1700000;
            ESP_LOGI(TAG, "TEST E/H recording real mic through upstream Opus loopback");
        } else if (strcmp(command, "tone") == 0) {
            if (app.GetDeviceState() != kDeviceStateWifiConfiguring ||
                !app.GetAudioService().IsIdle() || pressed_) {
                ESP_LOGW(TAG, "Tone requires idle Wi-Fi setup mode");
                return;
            }
            const bool ok = audio_.DiagnosticTone();
            ESP_LOGI(TAG, "TEST F digital I2S tone transfer %s; audible/power check PENDING",
                     ok ? "PASS" : "FAIL");
        } else if (strcmp(command, "factory-reset CONFIRM") == 0) {
            app.Schedule([]() {
                ESP_LOGW(TAG, "Erasing NVS settings and device UUID by explicit serial command");
                if (nvs_flash_erase() == ESP_OK)
                    esp_restart();
            });
        } else
            ESP_LOGI(TAG,
                     "Commands: handsfree on/off start stop cancel setup diag info colors home "
                     "face NAME mic "
                     "testaudio tone "
                     "volume N "
                     "wifi-reset CONFIRM factory-reset CONFIRM");
    }

    void ControlTask() {
        // GPIO0 is never driven. Ignore startup holds and require release before arming.
        vTaskDelay(pdMS_TO_TICKS(2500));
        Diagnostics();
        ESP_LOGI(TAG, "Hardware tests A-N PENDING until run on the connected board");
        int stable = 1, previous = gpio_get_level(POCKET_PTT_GPIO);
        int64_t changed = esp_timer_get_time();
        int64_t next_heap = changed + 30000000;
        while (true) {
            const int64_t now = esp_timer_get_time();
            const int raw = gpio_get_level(POCKET_PTT_GPIO);
            if (raw != previous) {
                previous = raw;
                changed = now;
            }
            if (now - changed >= 35000) {
                if (!armed_ && raw == 1) {
                    armed_ = true;
                    stable = 1;
                }
                if (armed_ && raw != stable) {
                    stable = raw;
                    if (!test_until_) {
                        if (raw == 0)
                            StartTalk();
                        else
                            StopTalk();
                    }
                }
            }
            uint8_t byte = 0;
            while (uart_read_bytes(UART_NUM_0, &byte, 1, 0) == 1) {
                if (byte == '\r' || byte == '\n') {
                    if (command_length_) {
                        command_[command_length_] = 0;
                        HandleCommand(command_.data());
                    }
                    command_length_ = 0;
                } else if (byte >= 32 && byte <= 126 && command_length_ < command_.size() - 1)
                    command_[command_length_++] = byte;
            }
            if (test_until_ && now >= test_until_) {
                test_peak_ = audio_.Peak();
                StopTalk();
                test_until_ = 0;
                test_finish_ = now + 3000000;
                ESP_LOGI(TAG,
                         "TEST E input frames captured=%lu; TEST H playback pending; compare "
                         "tx_frames after 3 seconds (before=%lu)",
                         audio_.InputFrames() - test_rx_before_, test_tx_before_);
            }
            if (test_finish_ && now >= test_finish_) {
                test_finish_ = 0;
                const uint32_t captured = audio_.InputFrames() - test_rx_before_;
                const uint32_t played = audio_.OutputFrames() - test_tx_before_;
                ESP_LOGI(TAG,
                         "TEST E digital capture %s frames=%lu measured_peak=%lu; acoustic mic "
                         "check PENDING",
                         captured && audio_.RxErrors() == test_rx_errors_ ? "PASS" : "FAIL",
                         captured, test_peak_);
                ESP_LOGI(
                    TAG,
                    "TEST H Opus loopback digital output %s frames=%lu; audible comparison PENDING",
                    played && audio_.TxErrors() == test_tx_errors_ ? "PASS" : "FAIL", played);
            }
            if (pressed_ && now - start_time_ > 30000000)
                StopTalk();
#if !CONFIG_POCKET_HARDWARE_DIAGNOSTICS
            PollHandsFree(now / 1000);
#endif
            if (now > next_heap) {
                Diagnostics();
                next_heap = now + 30000000;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
};

DECLARE_BOARD(PocketBoard);
