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
                              float output_gain = 0.08f) {
    const float value = static_cast<float>(sample) / 32768.0f;
    filtered = value - previous + 0.995f * filtered;
    previous = value;
    const float fraction = std::clamp(volume, 0, 100) / 100.0f;
    // Normal playback always supplies 0.08; only explicit diagnostics use a higher gain.
    const float cap = std::clamp(output_gain, 0.0f, 0.80f);
    const float output = std::clamp(filtered * fraction * fraction * cap, -cap, cap);
    return static_cast<int32_t>(output * 32767.0f) * 65536;
}
}  // namespace pocket
