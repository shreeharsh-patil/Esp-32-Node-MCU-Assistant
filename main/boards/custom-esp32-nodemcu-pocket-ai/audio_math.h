#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pocket {
inline int16_t ConvertMic(int32_t word, float& dc, float gain) {
    // INMP441 signed 24-bit PCM is MSB aligned in a 32-bit Philips I2S slot.
    const float sample = static_cast<float>(word >> 16);
    dc += 0.002f * (sample - dc);
    return static_cast<int16_t>(std::clamp((sample - dc) * gain, -32768.0f, 32767.0f));
}
inline int32_t ConvertSpeaker(int16_t sample, float& previous, float& filtered, int volume,
                              float output_gain = 0.80f, float voice_boost = 1.0f) {
    const float value = static_cast<float>(sample) / 32768.0f;
    filtered = value - previous + 0.995f * filtered;
    previous = value;
    const float fraction = std::clamp(volume, 0, 100) / 100.0f;
    // Owner-selected 80% ceiling; quiet diagnostics explicitly supply 0.08.
    const float cap = std::clamp(output_gain, 0.0f, 0.80f);
    const float boosted =
        filtered * fraction * fraction * cap * std::clamp(voice_boost, 1.0f, 2.0f);
    // Diagnostic tones retain their fixed gain. Voice has a soft knee so
    // louder words stay below the same limit without hard clipping.
    float output = std::clamp(boosted, -cap, cap);
    if (voice_boost > 1.0f && cap > 0.0f) {
        const float knee = cap * 0.6f;
        const float magnitude = std::abs(boosted);
        if (magnitude > knee) {
            const float progress = std::clamp((magnitude - knee) / (cap * 0.8f), 0.0f, 1.0f);
            const float limited = knee + (cap - knee) * (2.0f * progress - progress * progress);
            output = std::copysign(limited, boosted);
        }
    }
    return static_cast<int32_t>(output * 32767.0f) * 65536;
}
}  // namespace pocket
