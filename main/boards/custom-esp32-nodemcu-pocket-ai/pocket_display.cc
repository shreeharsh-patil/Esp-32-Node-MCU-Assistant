#include "pocket_display.h"
#include <esp_random.h>
#include <esp_wifi.h>
#include <algorithm>
#include <cstring>
#include "application.h"
#include "assets/lang_config.h"
#include "companion_text.h"
#include "config.h"
#include "lvgl_theme.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
static const char* TAG = "CompanionUI";
using pocket::face::Mode;
namespace {
uint64_t Now() { return esp_timer_get_time() / 1000; }
}  // namespace

PocketDisplay::PocketDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel,
                             PocketAudioCodec* audio)
    : SpiLcdDisplay(io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                    DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY, 0x0000),
      audio_(audio),
      engine_(esp_random()) {
    current_theme_ = LvglThemeManager::GetInstance().GetTheme("dark");
}

PocketDisplay::~PocketDisplay() {
    DisplayLockGuard lock(this);
    if (animation_)
        lv_timer_delete(animation_);
    if (display_)
        lv_display_remove_event_cb_with_user_data(display_, RefreshEvent, this);
    renderer_.reset();
    if (caption_)
        lv_obj_delete(caption_);
}

void PocketDisplay::RefreshEvent(lv_event_t* event) {
    auto* self = static_cast<PocketDisplay*>(lv_event_get_user_data(event));
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        self->refresh_started_ = esp_timer_get_time();
    } else if (lv_event_get_code(event) == LV_EVENT_RENDER_READY && self->refresh_started_) {
        const uint32_t elapsed = esp_timer_get_time() - self->refresh_started_;
        self->refresh_us_.store(elapsed);
        if (elapsed > self->max_refresh_us_.load())
            self->max_refresh_us_.store(elapsed);
        self->refreshes_.fetch_add(1);
    }
}

void PocketDisplay::SetupUI() {
    DisplayLockGuard lock(this);
    if (!lock || IsSetupUICalled() || !display_)
        return;
    Display::SetupUI();
    auto* screen = lv_display_get_screen_active(display_);
    // This board owns one screen. Discard any pre-existing objects and overlays
    // before creating the companion; never invoke the inherited chat SetupUI.
    lv_obj_clean(screen);
    lv_obj_clean(lv_display_get_layer_top(display_));
    lv_obj_clean(lv_display_get_layer_sys(display_));
    renderer_ = std::make_unique<pocket::face::Renderer>(screen);
    caption_ = lv_label_create(screen);
    lv_obj_set_pos(caption_, 10, 185);
    lv_obj_set_size(caption_, 260, 49);
    lv_obj_set_style_text_font(caption_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(caption_, lv_color_hex(pocket::face::kEye), 0);
    lv_obj_set_style_text_align(caption_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(caption_, LV_LABEL_LONG_DOT);
    lv_label_set_text_static(caption_, displayed_caption_);
    lv_obj_add_flag(caption_, LV_OBJ_FLAG_HIDDEN);
    boot_at_ = activity_at_ = Now();
    lv_display_add_event_cb(display_, RefreshEvent, LV_EVENT_ALL, this);
    animation_ = lv_timer_create(
        [](lv_timer_t* timer) {
            static_cast<PocketDisplay*>(lv_timer_get_user_data(timer))->Tick();
        },
        33, this);
    Tick();
    ESP_LOGI(
        TAG,
        "Landscape %dx%d, offsets=%d/%d, flipped=%d; target active 30fps, idle 15fps, sleepy 10fps",
        DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
        POCKET_LANDSCAPE_FLIPPED);
}

void PocketDisplay::Caption(const char* text, uint32_t duration_ms, bool dialogue) {
    pocket::CopyUtf8(caption_buffer_, text);
    caption_until_ = Now() + duration_ms;
    caption_is_dialogue_ = dialogue;
}

void PocketDisplay::Tick() {
    if (!renderer_)
        return;
    const uint64_t now = Now();
    const auto state = Application::GetInstance().GetDeviceState();
    if (last_tick_ && now - last_tick_ > 145)
        slow_ticks_.fetch_add(1);
    last_tick_ = now;
    ticks_.fetch_add(1);
    const bool listening = state == kDeviceStateListening || state == kDeviceStateAudioTesting;
    const uint32_t microphone = listening ? audio_->Rms() : 0;
    const uint32_t playback = audio_->PlaybackRms();
    const bool speaking =
        state == kDeviceStateSpeaking || state == kDeviceStateNotifying || playback > 0;
    if (state == kDeviceStateSpeaking || (listening && previous_state_ != state) ||
        state == kDeviceStateWifiConfiguring)
        turn_pose_.Reset();
    turn_pose_.Observe(now, listening, microphone);
    if (listening || speaking || state != previous_state_)
        activity_at_ = now;
    previous_state_ = state;
    if (turn_pose_.TimedOut(now)) {
        turn_pose_.Reset();
        error_ = true;
        Caption("Let's try again.\nReconnecting...", 15000);
    }
#if CONFIG_POCKET_HARDWARE_DIAGNOSTICS
    const bool setup = false;
    const bool disconnected = false;
#else
    const bool setup = state == kDeviceStateWifiConfiguring;
    const bool disconnected = !connected_ && state == kDeviceStateIdle && now - boot_at_ > 4000;
#endif
    Mode mode = Mode::Idle;
    if (error_ || disconnected || state == kDeviceStateFatalError)
        mode = Mode::Error;
    else if (speaking)
        mode = Mode::Speaking;
    else if (setup)
        mode = Mode::Setup;
    else if (turn_pose_.Thinking(now))
        mode = Mode::Thinking;
    else if (listening)
        mode = Mode::Listening;
    else if (state == kDeviceStateConnecting || state == kDeviceStateActivating ||
             state == kDeviceStateUpgrading)
        mode = Mode::Processing;
    else if (now < emotion_until_)
        mode = emotion_mode_;
    else if (power_save_ || now - activity_at_ > 120000)
        mode = Mode::Sleepy;
    if (speaking && !error_ && !listening && now < emotion_until_ && emotion_mode_ == Mode::Happy)
        mode = Mode::Happy;
    if (now - boot_at_ < 1650 && state != kDeviceStateFatalError)
        mode = Mode::Boot;
    if (listening || speaking)
        preview_until_ = 0;
    if (now < preview_until_)
        mode = preview_mode_;
    const char* text = "";
    if (color_test_)
        text = "RED   GREEN   BLUE\nLandscape · 280 x 240";
    else if (setup && setup_hint_[0] && !(wifi_feedback_ && now < caption_until_))
        text = setup_hint_;
    else if (now < caption_until_ && (!caption_is_dialogue_ || !subtitle_hidden_))
        text = caption_buffer_;
    else if (disconnected)
        text = "Connection lost.\nTrying again...";
    if (mode == Mode::Boot)
        text = "";
    const bool compact = text[0] != '\0';
    if (std::strcmp(displayed_caption_, text) != 0) {
        pocket::CopyUtf8(displayed_caption_, text);
        lv_label_set_text_static(caption_, displayed_caption_);
    }
    if (compact)
        lv_obj_remove_flag(caption_, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(caption_, LV_OBJ_FLAG_HIDDEN);
    renderer_->Draw(engine_.Step(now, mode, microphone, playback, compact));
    uint32_t period = mode == Mode::Sleepy ? 100 : mode == Mode::Idle ? 66 : 33;
    if (refresh_us_.load() > 28000)
        period = std::max<uint32_t>(period, 66);
    if (period != timer_period_ms_) {
        timer_period_ms_ = period;
        if (animation_)
            lv_timer_set_period(animation_, period);
    }
}

void PocketDisplay::SetStatus(const char* status) {
    ESP_LOGI(TAG, "Status: %s", status ? status : "");
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    if (status &&
        (std::strcmp(status, Lang::Strings::ERROR) == 0 || std::strstr(status, "error"))) {
        error_ = true;
        Caption("Something went wrong.\nPlease try again.", 15000);
    } else if (status && (std::strcmp(status, Lang::Strings::STANDBY) == 0 ||
                          std::strcmp(status, Lang::Strings::LISTENING) == 0 ||
                          std::strcmp(status, Lang::Strings::CONNECTING) == 0 ||
                          std::strcmp(status, Lang::Strings::ACTIVATION) == 0 ||
                          std::strcmp(status, Lang::Strings::SPEAKING) == 0 ||
                          std::strcmp(status, Lang::Strings::CONNECTION_SUCCESSFUL) == 0)) {
        error_ = false;
    }
}

void PocketDisplay::WifiSetup(const char* ssid, const char* key, bool instructions) {
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    if (instructions)
        snprintf(setup_hint_, sizeof(setup_hint_), "Open 192.168.4.1\nChoose 2.4GHz Wi-Fi");
    else
        snprintf(setup_hint_, sizeof(setup_hint_), "Join %.31s\nKey: %.16s", ssid, key);
    wifi_feedback_ = false;
    error_ = false;
    connected_ = false;
}

void PocketDisplay::WifiFeedback(const char* status, const char* detail) {
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    char text[128];
    snprintf(text, sizeof(text), "%s%s%s", status, detail && detail[0] ? "\n" : "",
             detail ? detail : "");
    Caption(text, std::strcmp(status, "Connecting to Wi-Fi") == 0 ||
                          std::strcmp(status, "Reconnecting") == 0
                      ? 30000
                      : 6000);
    wifi_feedback_ = true;
    error_ = std::strstr(status, "failed") != nullptr;
    connected_ = std::strcmp(status, "Connected") == 0;
    if (connected_ || std::strcmp(status, "Connecting to Wi-Fi") == 0) {
        setup_hint_[0] = 0;
        turn_pose_.Reset();
    }
}

void PocketDisplay::ShowNotification(const std::string& message, int duration_ms) {
    ShowNotification(message.c_str(), duration_ms);
}
void PocketDisplay::ShowNotification(const char* message, int duration_ms) {
    ESP_LOGI(TAG, "Notification: %s", message ? message : "");
    DisplayLockGuard lock(this);
    if (!lock || pocket::TechnicalMessage(message))
        return;
    if (message && std::strstr(message, "Volume"))
        Caption("Volume updated", std::clamp(duration_ms, 500, 10000));
}

void PocketDisplay::SetChatMessage(const char* role, const char* content) {
    const bool dialogue =
        role && (std::strcmp(role, "user") == 0 || std::strcmp(role, "assistant") == 0);
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    if (dialogue) {
        if (std::strcmp(role, "user") == 0 && content && content[0] &&
            Application::GetInstance().GetDeviceState() == kDeviceStateListening)
            turn_pose_.AwaitReply(Now());
        Caption(content, 10000, true);
        return;
    }
    ESP_LOGI(TAG, "System: %s", content ? content : "");
    const auto state = Application::GetInstance().GetDeviceState();
    if (state == kDeviceStateStarting || pocket::TechnicalMessage(content))
        return;
    if (!content || !content[0]) {
        caption_until_ = 0;
        return;
    }
    if (state == kDeviceStateWifiConfiguring) {
        const char* ssid = std::strstr(content, Lang::Strings::CONNECT_TO_HOTSPOT);
        const char* url = std::strstr(content, Lang::Strings::ACCESS_VIA_BROWSER);
        if (ssid && url && url > ssid) {
            ssid += std::strlen(Lang::Strings::CONNECT_TO_HOTSPOT);
            const int length = std::min<int>(url - ssid, 32);
            url += std::strlen(Lang::Strings::ACCESS_VIA_BROWSER);
            if (std::strncmp(url, "http://", 7) == 0)
                url += 7;
            snprintf(setup_hint_, sizeof(setup_hint_), "Join %.*s\nOpen %.48s", length, ssid, url);
        }
    } else if (state == kDeviceStateActivating) {
        // Raw service details stay on serial; SetActivationCode displays the real code.
    } else if (error_) {
        Caption("Let's try again.\nReconnecting...", 15000);
    }
}

void PocketDisplay::SetActivationCode(const std::string& code) {
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    char text[96];
    snprintf(text, sizeof(text), "Pair at xiaozhi.me\n%.32s", code.c_str());
    Caption(text, 600000);
}
void PocketDisplay::ClearChatMessages() {
    DisplayLockGuard lock(this);
    if (lock)
        caption_until_ = 0;
}
void PocketDisplay::SetEmotion(const char* emotion) {
    DisplayLockGuard lock(this);
    if (!lock)
        return;
    emotion_mode_ = Mode::Idle;
    if (emotion) {
        if (std::strcmp(emotion, "happy") == 0 || std::strcmp(emotion, "laughing") == 0 ||
            std::strcmp(emotion, "loving") == 0 || std::strcmp(emotion, "funny") == 0)
            emotion_mode_ = Mode::Happy;
        else if (std::strcmp(emotion, "confused") == 0)
            emotion_mode_ = Mode::Confused;
        else if (std::strcmp(emotion, "surprised") == 0)
            emotion_mode_ = Mode::Surprised;
        else if (std::strcmp(emotion, "embarrassed") == 0 || std::strcmp(emotion, "shy") == 0)
            emotion_mode_ = Mode::Shy;
        else if (std::strcmp(emotion, "curious") == 0)
            emotion_mode_ = Mode::Curious;
        else if (std::strcmp(emotion, "sleepy") == 0)
            emotion_mode_ = Mode::Sleepy;
    }
    emotion_until_ = Now() + 5500;
}
void PocketDisplay::SetTheme(Theme* theme) {
    DisplayLockGuard lock(this);
    if (lock && theme)
        current_theme_ = theme;
}
void PocketDisplay::UpdateStatusBar(bool all) {
    wifi_ap_record_t access_point = {};
    const bool connected = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
    DisplayLockGuard lock(this);
    if (lock)
        connected_ = connected;
    (void)all;
}
void PocketDisplay::SetPowerSaveMode(bool on) {
    DisplayLockGuard lock(this);
    if (lock)
        power_save_ = on;
}
void PocketDisplay::SetHideSubtitle(bool hide) {
    DisplayLockGuard lock(this);
    if (lock)
        subtitle_hidden_ = hide;
}
void PocketDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) { (void)image; }
void PocketDisplay::Thinking(bool on) {
    DisplayLockGuard lock(this);
    if (lock) {
        if (on)
            turn_pose_.AwaitReply(Now());
        else
            turn_pose_.Reset();
    }
}
void PocketDisplay::ColorTest(bool show) {
    DisplayLockGuard lock(this);
    if (!lock || !renderer_)
        return;
    color_test_ = show;
    renderer_->ColorTest(show);
    if (!show)
        caption_until_ = 0;
}
bool PocketDisplay::PreviewExpression(const char* expression) {
    DisplayLockGuard lock(this);
    if (!lock || !expression)
        return false;
    const auto state = Application::GetInstance().GetDeviceState();
    if (state != kDeviceStateIdle && state != kDeviceStateWifiConfiguring)
        return false;
    const struct {
        const char* name;
        Mode mode;
    } choices[] = {{"boot", Mode::Boot},
                   {"idle", Mode::Idle},
                   {"listening", Mode::Listening},
                   {"thinking", Mode::Thinking},
                   {"speaking", Mode::Speaking},
                   {"happy", Mode::Happy},
                   {"confused", Mode::Confused},
                   {"sleepy", Mode::Sleepy},
                   {"error", Mode::Error},
                   {"curious", Mode::Curious},
                   {"shy", Mode::Shy},
                   {"surprised", Mode::Surprised},
                   {"processing", Mode::Processing}};
    for (const auto& choice : choices) {
        if (std::strcmp(expression, choice.name) == 0) {
            preview_mode_ = choice.mode;
            preview_until_ = Now() + 10000;
            ESP_LOGI(TAG,
                     "Face preview %s for 10s; service state and audio unchanged; no simulated "
                     "mouth amplitude",
                     expression);
            return true;
        }
    }
    return false;
}
void PocketDisplay::PrintUiDiagnostics() const {
    ESP_LOGI(TAG,
             "UI ticks=%lu refreshes=%lu slow_ticks=%lu latest_refresh_us=%lu max_refresh_us=%lu; "
             "strip=%dB",
             ticks_.load(), refreshes_.load(), slow_ticks_.load(), refresh_us_.load(),
             max_refresh_us_.load(), DISPLAY_WIDTH * 20 * 2);
}
