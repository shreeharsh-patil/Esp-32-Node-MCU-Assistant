#pragma once
#include <atomic>
#include <memory>
#include "display/lcd_display.h"
#include "face_animation.h"
#include "face_renderer.h"
#include "pocket_audio_codec.h"

class PocketDisplay : public SpiLcdDisplay {
public:
    PocketDisplay(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel,
                  PocketAudioCodec* audio);
    ~PocketDisplay() override;
    void SetupUI() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* message, int duration_ms = 3000) override;
    void ShowNotification(const std::string& message, int duration_ms = 3000) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetEmotion(const char* emotion) override;
    void SetTheme(Theme* theme) override;
    void UpdateStatusBar(bool all = false) override;
    void SetPowerSaveMode(bool on) override;
    void SetHideSubtitle(bool hide) override;
    void SetActivationCode(const std::string& code) override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void ColorTest(bool show);
    void Thinking(bool on);
    void WifiSetup(const char* ssid, const char* key, bool instructions);
    void WifiFeedback(const char* status, const char* detail = "");
    void PrintUiDiagnostics() const;
    bool PreviewExpression(const char* expression);
    bool AddTextGlyphs(const std::vector<TextGlyph>& glyphs, uint8_t bpp) override { return false; }
    bool SupportsGuiOperations() const override { return false; }

private:
    PocketAudioCodec* audio_;
    pocket::face::Engine engine_;
    pocket::face::TurnPose turn_pose_;
    std::unique_ptr<pocket::face::Renderer> renderer_;
    lv_obj_t* caption_ = nullptr;
    lv_timer_t* animation_ = nullptr;
    char caption_buffer_[513] = {}, displayed_caption_[513] = {};
    char setup_hint_[128] = {};
    bool color_test_ = false, error_ = false;
    bool connected_ = false, power_save_ = false, subtitle_hidden_ = false;
    bool caption_is_dialogue_ = false;
    bool wifi_feedback_ = false;
    uint64_t boot_at_ = 0, caption_until_ = 0;
    uint64_t emotion_until_ = 0, preview_until_ = 0, activity_at_ = 0, last_tick_ = 0;
    pocket::face::Mode emotion_mode_ = pocket::face::Mode::Idle;
    pocket::face::Mode preview_mode_ = pocket::face::Mode::Idle;
    int previous_state_ = -1;
    uint32_t timer_period_ms_ = 33;
    std::atomic<uint32_t> ticks_{0}, refreshes_{0}, slow_ticks_{0};
    std::atomic<uint32_t> refresh_us_{0}, max_refresh_us_{0};
    uint64_t refresh_started_ = 0;
    void Tick();
    void Caption(const char* text, uint32_t duration_ms = 8000, bool dialogue = false);
    static void RefreshEvent(lv_event_t* event);
};
