#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pocket {
inline float FilterSpeaker(int16_t sample, float& previous, float& filtered) {
    const float value = static_cast<float>(sample) / 32768.0f;
    filtered = value - previous + 0.995f * filtered;
    previous = value;
    return filtered;
}

inline float VoiceBlockGain(float peak, float& gain, int volume, int frames, float output_gain,
                            float voice_boost) {
    const float fraction = std::clamp(volume, 0, 100) / 100.0f;
    const float cap = std::clamp(output_gain, 0.0f, 0.80f);
    const float requested = fraction * fraction * cap * std::clamp(voice_boost, 1.0f, 2.0f);
    // Preserve quiet-word gain; reduce a whole 8 ms block before its peaks reach
    // the amplifier. Slow release (~200 ms) avoids reshaping every loud sample.
    const float ceiling = std::min(cap, 0.65f);
    const float target = peak > 0 ? std::min(requested, ceiling / peak) : requested;
    const float release = std::clamp(frames / 3200.0f, 0.0f, 1.0f);
    gain = std::min(target, gain + (target - gain) * release);
    return gain;
}

inline int32_t PackSpeaker(float value, float ceiling) {
    const float output = std::clamp(value, -ceiling, ceiling);
    return static_cast<int32_t>(output * 32767.0f) * 65536;
}

inline int16_t ConvertMic(int32_t word, float& dc, float gain) {
    // INMP441 signed 24-bit PCM is MSB aligned in a 32-bit Philips I2S slot.
    const float sample = static_cast<float>(word >> 16);
    dc += 0.002f * (sample - dc);
    return static_cast<int16_t>(std::clamp((sample - dc) * gain, -32768.0f, 32767.0f));
}
inline int32_t ConvertSpeaker(int16_t sample, float& previous, float& filtered, int volume,
                              float output_gain = 0.80f) {
    FilterSpeaker(sample, previous, filtered);
    const float fraction = std::clamp(volume, 0, 100) / 100.0f;
    // Owner-selected 80% ceiling; quiet diagnostics explicitly supply 0.08.
    const float cap = std::clamp(output_gain, 0.0f, 0.80f);
    return PackSpeaker(filtered * fraction * fraction * cap, cap);
}
}  // namespace pocket
