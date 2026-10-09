#include "pocket_audio_codec.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include "application.h"
#include "audio_math.h"
#include "config.h"
#include "settings.h"

static const char* TAG = "PocketAudio";

PocketAudioCodec::PocketAudioCodec() : application_(&Application::GetInstance()) {
    duplex_ = true;
    input_sample_rate_ = AUDIO_INPUT_SAMPLE_RATE;
    output_sample_rate_ = AUDIO_OUTPUT_SAMPLE_RATE;
    output_volume_ = 20;
    input_gain_ = 4.0f;
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(0, I2S_ROLE_MASTER);
    channels.dma_desc_num = 6;
    channels.dma_frame_num = kChunk;
    channels.auto_clear_after_cb = true;
    esp_err_t err = i2s_new_channel(&channels, &tx_handle_, &rx_handle_);
    i2s_std_config_t standard = {};
    standard.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);
    standard.slot_cfg =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    standard.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    standard.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    standard.gpio_cfg.bclk = AUDIO_BCLK;
    standard.gpio_cfg.ws = AUDIO_WS;
    standard.gpio_cfg.dout = AUDIO_SPEAKER_DOUT;
    standard.gpio_cfg.din = AUDIO_MIC_DIN;
    if (err == ESP_OK)
        err = i2s_channel_init_std_mode(tx_handle_, &standard);
    if (err == ESP_OK)
        err = i2s_channel_init_std_mode(rx_handle_, &standard);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S initialization FAIL: %s", esp_err_to_name(err));
        if (tx_handle_)
            i2s_del_channel(tx_handle_);
        if (rx_handle_)
            i2s_del_channel(rx_handle_);
        tx_handle_ = rx_handle_ = nullptr;
        return;
    }
    i2s_event_callbacks_t callbacks = {};
    callbacks.on_recv_q_ovf = [](i2s_chan_handle_t, i2s_event_data_t*, void* context) {
        auto* codec = static_cast<PocketAudioCodec*>(context);
        // Input stays enabled for upstream's 15-second idle grace period.
        // GetDeviceState reads an atomic enum; the cached singleton is already
        // initialized before the IRQ is enabled. No locks or allocation here.
        const auto state = codec->application_->GetDeviceState();
        if (codec->capture_.load() &&
            (state == kDeviceStateListening || state == kDeviceStateAudioTesting))
            codec->overruns_.fetch_add(1);
        return false;
    };
    i2s_channel_register_event_callback(rx_handle_, &callbacks, this);
    ready_ = true;
    ESP_LOGI(TAG, "I2S driver init PASS; physical capture/clocks/playback PENDING");
}

void PocketAudioCodec::Start() {
    if (!ready_ || started_)
        return;
    Settings settings("audio", false);
    output_volume_ = std::clamp(static_cast<int>(settings.GetInt("output_volume", 20)), 0, 100);
    safe_volume_.store(output_volume_);
    esp_err_t err = i2s_channel_enable(tx_handle_);
    if (err == ESP_OK)
        err = i2s_channel_enable(rx_handle_);
    started_ = err == ESP_OK;
    ready_ = started_;
    ESP_LOGI(TAG, "Shared 16kHz/32-bit stereo clock start: %s", esp_err_to_name(err));
}

void PocketAudioCodec::EnableInput(bool enable) {
    if (enable && !capture_.load() && started_) {
        size_t discarded = 0;
        // Discard bounded stale DMA data before a new recording. Single input owner.
        std::array<int32_t, kChunk * 2> discard{};
        for (int i = 0; i < 6; ++i)
            i2s_channel_read(rx_handle_, discard.data(), sizeof(discard), &discarded, 0);
        mic_dc_ = 0;
    }
    capture_.store(enable && ready_);
    if (!enable)
        rms_.store(0);
    AudioCodec::EnableInput(enable && ready_);
}

void PocketAudioCodec::EnableOutput(bool enable) {
    playback_.store(enable && ready_);
    if (!enable)
        playback_rms_.store(0);
    AudioCodec::EnableOutput(enable && ready_);
}

void PocketAudioCodec::SetOutputVolume(int volume) {
    const int clamped = std::clamp(volume, 0, 100);
    safe_volume_.store(clamped);
    AudioCodec::SetOutputVolume(clamped);
}

void PocketAudioCodec::SetInputGain(float gain) {
    if (!std::isfinite(gain))
        return;
    gain = std::clamp(gain, 1.0f, 8.0f);
    mic_gain_.store(gain);
    AudioCodec::SetInputGain(gain);
}

int PocketAudioCodec::Read(int16_t* dest, int samples) {
    if (!ready_ || !capture_.load())
        return 0;
    uint64_t squares = 0;
    uint32_t peak = 0;
    int done = 0;
    while (done < samples && capture_.load()) {
        const int frames = std::min(kChunk, samples - done);
        size_t bytes = 0;
        const auto err = i2s_channel_read(rx_handle_, rx_words_.data(), frames * 8, &bytes, 100);
        if (err != ESP_OK || bytes != static_cast<size_t>(frames * 8)) {
            rx_errors_.fetch_add(1);
            std::fill(dest, dest + samples, 0);
            return 0;
        }
        for (int i = 0; i < frames; ++i) {
            const int16_t value = pocket::ConvertMic(rx_words_[i * 2], mic_dc_, mic_gain_.load());
            dest[done + i] = value;
            squares += static_cast<int64_t>(value) * value;
            peak = std::max(peak, static_cast<uint32_t>(std::abs(static_cast<int>(value))));
        }
        done += frames;
    }
    if (done != samples)
        return 0;
    rms_.store(static_cast<uint32_t>(std::sqrt(static_cast<double>(squares) / samples)));
    peak_.store(peak);
    input_frames_.fetch_add(done);
    return done;
}

int PocketAudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (!ready_ || !playback_.load())
        return 0;
    int done = 0;
    while (done < samples && playback_.load()) {
        const int frames = std::min(kChunk, samples - done);
        uint64_t squares = 0;
        for (int i = 0; i < frames; ++i) {
            const int64_t sample = data[done + i];
            squares += sample * sample;
            const int32_t word = pocket::ConvertSpeaker(data[done + i], speaker_previous_,
                                                        speaker_filtered_, safe_volume_.load());
            tx_words_[i * 2] = tx_words_[i * 2 + 1] = word;
        }
        size_t bytes = 0;
        const auto err = i2s_channel_write(tx_handle_, tx_words_.data(), frames * 8, &bytes, 150);
        if (err != ESP_OK || bytes != static_cast<size_t>(frames * 8)) {
            tx_errors_.fetch_add(1);
            playback_rms_.store(0);
            return done;
        }
        playback_rms_.store(
            static_cast<uint32_t>(std::sqrt(static_cast<double>(squares) / frames)));
        playback_at_ms_.store(static_cast<uint32_t>(esp_timer_get_time() / 1000));
        done += frames;
    }
    output_frames_.fetch_add(done);
    return done;
}

uint32_t PocketAudioCodec::PlaybackRms() const {
    const uint32_t now = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (!playback_.load() || safe_volume_.load() == 0 || now - playback_at_ms_.load() > 100)
        return 0;
    return playback_rms_.load();
}

bool PocketAudioCodec::DiagnosticTone() {
    EnableOutput(true);
    std::array<int16_t, 160> samples{};
    bool ok = ready_;
    for (int block = 0; block < 12 && ok; ++block) {
        for (int i = 0; i < 160; ++i) {
            const int n = block * 160 + i;
            const float ramp = std::min(1.0f, std::min(n, 1919 - n) / 160.0f);
            samples[i] = static_cast<int16_t>(6000 * ramp * std::sin(n * 6.2831853f * 440 / 16000));
        }
        ok = Write(samples.data(), samples.size()) == static_cast<int>(samples.size());
    }
    EnableOutput(false);
    return ok;
}
