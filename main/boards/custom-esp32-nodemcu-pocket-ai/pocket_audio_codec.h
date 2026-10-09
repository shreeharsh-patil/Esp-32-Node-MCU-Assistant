#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include "audio_codec.h"

class Application;

class PocketAudioCodec : public AudioCodec {
public:
    PocketAudioCodec();
    void Start() override;
    void EnableInput(bool enable) override;
    void EnableOutput(bool enable) override;
    void SetOutputVolume(int volume) override;
    void SetInputGain(float gain) override;
    bool Ready() const { return ready_; }
    uint32_t Rms() const { return rms_.load(); }
    uint32_t Peak() const { return peak_.load(); }
    uint32_t RxErrors() const { return rx_errors_.load(); }
    uint32_t TxErrors() const { return tx_errors_.load(); }
    uint32_t Overruns() const { return overruns_.load(); }
    uint32_t InputFrames() const { return input_frames_.load(); }
    uint32_t OutputFrames() const { return output_frames_.load(); }
    // PCM accepted by I2S, before safety gain. Not an acoustic measurement.
    uint32_t PlaybackRms() const;
    // Called by the board control task only while upstream playback is idle.
    bool DiagnosticTone();

protected:
    int Read(int16_t* dest, int samples) override;
    int Write(const int16_t* data, int samples) override;

private:
    Application* application_;
    static constexpr int kChunk = 128;
    bool ready_ = false;
    bool started_ = false;
    std::atomic<int> safe_volume_{20};
    std::atomic<float> mic_gain_{4.0f};
    std::atomic<bool> capture_{false}, playback_{false};
    std::atomic<uint32_t> rms_{0}, peak_{0}, rx_errors_{0}, tx_errors_{0}, overruns_{0};
    std::atomic<uint32_t> input_frames_{0}, output_frames_{0};
    std::atomic<uint32_t> playback_rms_{0}, playback_at_ms_{0};
    std::array<int32_t, kChunk * 2> rx_words_{}, tx_words_{};
    float mic_dc_ = 0, speaker_previous_ = 0, speaker_filtered_ = 0;
    std::mutex output_mutex_;
};
