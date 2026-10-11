#include "pocket_audio_codec.h"
#include <driver/uart.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
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
    // 7 queued blocks = 56 ms scheduling margin, +4096 B for paired RX/TX.
    channels.dma_desc_num = kDmaDescriptors;
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
        const bool active = codec->diagnostic_capture_.load() || state == kDeviceStateListening ||
                            state == kDeviceStateAudioTesting;
        // Even within the service's idle grace, a new session must discard
        // unread idle data. Count drops only after its first fresh read.
        if (!active)
            codec->fresh_capture_.store(true);
        else if (codec->capture_.load() && !codec->fresh_capture_.load())
            codec->overruns_.fetch_add(1);
        return false;
    };
    err = i2s_channel_register_event_callback(rx_handle_, &callbacks, this);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX overflow callback registration FAIL: %s", esp_err_to_name(err));
        i2s_del_channel(tx_handle_);
        i2s_del_channel(rx_handle_);
        tx_handle_ = rx_handle_ = nullptr;
        return;
    }
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
    std::lock_guard<std::mutex> lock(input_mutex_);
    // Flush at the first read, after upstream warmup and input-enable logging.
    if (enable && !capture_.load())
        fresh_capture_.store(true);
    capture_.store(enable && ready_);
    if (!enable)
        rms_.store(0);
    AudioCodec::EnableInput(enable && ready_);
}

void PocketAudioCodec::EnableOutput(bool enable) {
    if (!enable && diagnostic_playback_.load())
        return;  // The service's idle power timer must not mute an explicit tone.
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
    std::lock_guard<std::mutex> lock(input_mutex_);
    return ReadCaptured(dest, samples);
}

int PocketAudioCodec::ReadCaptured(int16_t* dest, int samples) {
    if (!ready_ || !capture_.load())
        return 0;
    if (samples <= 0)
        return 0;
    if (fresh_capture_.exchange(false)) {
        size_t discarded = 0;
        for (int i = 0; i < kDmaDescriptors; ++i)
            i2s_channel_read(rx_handle_, rx_words_.data(), sizeof(rx_words_), &discarded, 0);
        mic_dc_ = 0;
    }
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
            if (value == INT16_MIN || value == INT16_MAX)
                mic_clipped_.fetch_add(1);
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
    return WriteSamples(data, samples, 0.08f);
}

int PocketAudioCodec::WriteSamples(const int16_t* data, int samples, float output_gain) {
    std::lock_guard<std::mutex> lock(output_mutex_);
    if (!ready_ || !playback_.load())
        return 0;
    int done = 0;
    while (done < samples && playback_.load()) {
        const int frames = std::min(kChunk, samples - done);
        uint64_t squares = 0;
        uint64_t tx_squares = 0;
        uint32_t tx_peak = 0;
        for (int i = 0; i < frames; ++i) {
            const int64_t sample = data[done + i];
            squares += sample * sample;
            const int32_t word =
                pocket::ConvertSpeaker(data[done + i], speaker_previous_, speaker_filtered_,
                                       safe_volume_.load(), output_gain);
            tx_words_[i * 2] = tx_words_[i * 2 + 1] = word;
            const int64_t pcm = word / 65536;
            tx_squares += pcm * pcm;
            tx_peak = std::max(tx_peak, static_cast<uint32_t>(std::abs(pcm)));
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
        tx_rms_.store(static_cast<uint32_t>(std::sqrt(static_cast<double>(tx_squares) / frames)));
        tx_peak_.store(tx_peak);
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

bool PocketAudioCodec::DiagnosticTone(bool extended, bool loud) {
    PrintSignalExpectations();
    const bool long_tone = extended || loud;
    const int duration_ms = long_tone ? 5000 : 300;
    const int source_peak = long_tone ? 24000 : 6000;
    const float output_gain = loud ? 0.80f : 0.08f;
    const int total_frames = duration_ms * 16;
    const int saved_volume = safe_volume_.exchange(long_tone ? 100 : 50);
    const bool saved_output = playback_.load();
    diagnostic_playback_.store(true);
    speaker_previous_ = speaker_filtered_ = 0;
    EnableOutput(true);
    std::array<int16_t, 160> samples{};
    bool ok = ready_;
    uint32_t peak = 0;
    int32_t example_left = 0, example_right = 0;
    ESP_LOGI(TAG, "TONE starting 440Hz %dms; temporary volume=%d; digital cap=%d%%", duration_ms,
             safe_volume_.load(), loud ? 80 : 8);
    for (int block = 0; block < total_frames / 160 && ok; ++block) {
        for (int i = 0; i < 160; ++i) {
            const int n = block * 160 + i;
            const float ramp = std::min(1.0f, std::min(n, total_frames - 1 - n) / 160.0f);
            samples[i] =
                static_cast<int16_t>(source_peak * ramp * std::sin(n * 6.2831853f * 440 / 16000));
        }
        ok = WriteSamples(samples.data(), samples.size(), output_gain) ==
             static_cast<int>(samples.size());
        peak = std::max(peak, TxPeak());
        if (block == 1 && ok) {
            example_left = tx_words_[0];
            example_right = tx_words_[1];
        }
    }
    // Push a full DMA ring of silence so the queued tone leaves the wire before return.
    samples.fill(0);
    for (int i = 0; i < kDmaDescriptors && ok; ++i)
        ok = WriteSamples(samples.data(), samples.size(), output_gain) ==
             static_cast<int>(samples.size());
    ESP_LOGI(TAG, "TONE 440Hz %dms fixed diagnostic gain; TX PCM16 peak=%lu (after gain)",
             duration_ms, peak);
    ESP_LOGI(TAG, "TONE example TX L=0x%08lx R=0x%08lx signed_pcm16=%ld",
             static_cast<uint32_t>(example_left), static_cast<uint32_t>(example_right),
             example_left / 65536);
    safe_volume_.store(saved_volume);
    diagnostic_playback_.store(false);
    EnableOutput(saved_output);
    return ok;
}

void PocketAudioCodec::PrintSignalExpectations() const {
    ESP_LOGI(TAG, "SIGNALS BCLK GPIO%d=1024000Hz; WS GPIO%d=16000Hz; DIN GPIO%d; MIC GPIO%d",
             AUDIO_BCLK, AUDIO_WS, AUDIO_SPEAKER_DOUT, AUDIO_MIC_DIN);
    ESP_LOGI(TAG, "Philips I2S: 32-bit L/R MSB first, 1-bit delay, identical TX slots; 3V3 logic");
    ESP_LOGI(TAG, "VIN/SD_MODE/physical I2S/amplifier/audible output PENDING electrical checks");
}

bool PocketAudioCodec::DiagnosticRecord() {
    constexpr int kSamples = 80000;
    if (!ready_)
        return false;
    // PCM hex needs ~68 KB/s; 115200 baud carries only 11.5 KB/s. A bounded
    // 512-byte scratch buffer streams at 921600 baud instead of allocating
    // 160 KB on a no-PSRAM ESP32. The host utility changes speed and ACKs.
    ESP_LOGI(TAG, "MIC use pocket_serial_capture.py; 5 seconds PCM, no speaker replay");
    printf("PCM_UART baud=921600\n");
    fflush(stdout);
    uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
    bool ok = uart_set_baudrate(UART_NUM_0, 921600) == ESP_OK;
    uint8_t ack = 0;
    ok = ok && uart_read_bytes(UART_NUM_0, &ack, 1, pdMS_TO_TICKS(2000)) == 1 && ack == 6;
    const bool exporting = ok;
    if (!ok) {
        // Recover even if no host is following the temporary speed change.
        printf("PCM_UART baud=115200\n");
        fflush(stdout);
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
        vTaskDelay(pdMS_TO_TICKS(100));
        if (uart_set_baudrate(UART_NUM_0, 115200) != ESP_OK)
            return false;
        ESP_LOGW(TAG, "MIC no host ACK; console restored to 115200, levels only (no PCM export)");
    }
    ok = true;
    std::array<int16_t, 256> pcm{};
    uint64_t squares = 0;
    uint32_t peak = 0, crc = 0;
    int captured = 0, next_stats = 4000;
    uint32_t before_errors = RxErrors(), before_overruns = Overruns(), before_clips = MicClipped();
    ESP_LOGI(TAG, "MIC speak now for 5 seconds");
    if (exporting)
        printf("PCM_BEGIN rate=16000 samples=80000\n");
    {
        // Exclusive reader while the idle input task may process its power-stop request.
        std::lock_guard<std::mutex> lock(input_mutex_);
        const bool saved_capture = capture_.load();
        diagnostic_capture_.store(true);
        capture_.store(true);
        fresh_capture_.store(true);
        while (captured < kSamples && ok) {
            const int count = std::min(static_cast<int>(pcm.size()), kSamples - captured);
            ok = ReadCaptured(pcm.data(), count) == count;
            if (!ok)
                break;
            crc = esp_rom_crc32_le(crc, reinterpret_cast<const uint8_t*>(pcm.data()), count * 2);
            for (int i = 0; i < count; ++i) {
                const int64_t value = pcm[i];
                squares += value * value;
                peak = std::max(peak, static_cast<uint32_t>(std::abs(value)));
            }
            for (int offset = 0; exporting && offset < count; offset += 64) {
                char hex[257];
                static constexpr char digits[] = "0123456789abcdef";
                for (int i = 0; i < 64; ++i) {
                    const uint16_t word = static_cast<uint16_t>(pcm[offset + i]);
                    hex[i * 4] = digits[(word >> 4) & 15];
                    hex[i * 4 + 1] = digits[word & 15];
                    hex[i * 4 + 2] = digits[(word >> 12) & 15];
                    hex[i * 4 + 3] = digits[(word >> 8) & 15];
                }
                hex[256] = 0;
                printf("PCM %d %s\n", captured + offset, hex);
            }
            captured += count;
            if (captured >= next_stats) {
                ESP_LOGI(
                    TAG,
                    "MIC frames=%d rms=%lu peak=%lu clipped_pct=%.2f overruns=%lu rx_errors=%lu",
                    captured, Rms(), Peak(), 100.0 * (MicClipped() - before_clips) / captured,
                    Overruns() - before_overruns, RxErrors() - before_errors);
                next_stats += 4000;
            }
        }
        capture_.store(saved_capture);
        fresh_capture_.store(true);
        diagnostic_capture_.store(false);
    }
    if (exporting) {
        printf("PCM_END samples=%d rx_errors=%lu overruns=%lu clipped=%lu crc32=%08lx\n", captured,
               RxErrors() - before_errors, Overruns() - before_overruns,
               MicClipped() - before_clips, crc);
        printf("PCM_UART baud=115200\n");
        fflush(stdout);
        uart_wait_tx_done(UART_NUM_0, pdMS_TO_TICKS(200));
        vTaskDelay(pdMS_TO_TICKS(100));
        ok = uart_set_baudrate(UART_NUM_0, 115200) == ESP_OK && ok;
    }
    ESP_LOGI(
        TAG, "MIC total_frames=%d rms=%lu peak=%lu clipped_pct=%.2f overruns=%lu rx_errors=%lu",
        captured,
        captured ? static_cast<uint32_t>(std::sqrt(static_cast<double>(squares) / captured)) : 0,
        peak, captured ? 100.0 * (MicClipped() - before_clips) / captured : 0,
        Overruns() - before_overruns, RxErrors() - before_errors);
    return ok && RxErrors() == before_errors && Overruns() == before_overruns;
}
