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
inline int32_t ConvertSpeaker(int16_t sample, float& previous, float& filtered, int volume) {
    const float value = static_cast<float>(sample) / 32768.0f;
    filtered = value - previous + 0.995f * filtered;
    previous = value;
    const float fraction = std::clamp(volume, 0, 100) / 100.0f;
    // Absolute output cap remains even if MCP or persisted settings request 100%.
    const float output = std::clamp(filtered * fraction * fraction * 0.08f, -0.08f, 0.08f);
    return static_cast<int32_t>(output * 32767.0f) * 65536;
}
}  // namespace pocket
