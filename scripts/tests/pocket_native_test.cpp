#include <array>
#include <cassert>
#include <climits>
#include <cmath>
#include "audio_math.h"
#include "websocket_audio_frame.h"

int main() {
    float dc = 0;
    assert(pocket::ConvertMic(0, dc, 4) == 0);
    dc = 0;
    assert(pocket::ConvertMic(0x00100000, dc, 1) > 0);
    dc = 0;
    assert(pocket::ConvertMic(-0x00100000, dc, 1) < 0);
    dc = 0;
    assert(pocket::ConvertMic(INT32_MAX, dc, 8) == INT16_MAX);
    dc = 0;
    assert(pocket::ConvertMic(INT32_MIN, dc, 8) == INT16_MIN);
    dc = 0;
    int16_t sample = 0;
    for (int i = 0; i < 10000; ++i)
        sample = pocket::ConvertMic(0x10000000, dc, 1);
    assert(std::abs(sample) < 2);
    float previous = 0, filtered = 0;
    assert(pocket::ConvertSpeaker(INT16_MAX, previous, filtered, 0) == 0);
    for (int volume : {-100, 0, 20, 100, 1000}) {
        previous = filtered = 0;
        for (int i = 0; i < 10000; ++i) {
            const int16_t value = i % 2 ? INT16_MAX : INT16_MIN;
            const int64_t output = pocket::ConvertSpeaker(value, previous, filtered, volume);
            assert(std::abs(output) <= static_cast<int64_t>(2622) * 65536);
            assert(output % 65536 == 0);
            if (volume <= 0)
                assert(output == 0);
        }
    }
    xiaozhi::WebsocketAudioFrame frame;
    std::array<uint8_t, 18> v2 = {0,    2,    0,    0, 0, 0, 0, 0,    0x12,
                                  0x34, 0x56, 0x78, 0, 0, 0, 2, 0xab, 0xcd};
    const auto original = v2;
    assert(xiaozhi::ParseWebsocketAudio(v2.data(), v2.size(), 2, frame));
    assert(frame.size == 2 && frame.timestamp == 0x12345678 && frame.payload[0] == 0xab);
    assert(v2 == original);
    for (size_t n = 0; n < v2.size(); ++n)
        assert(!xiaozhi::ParseWebsocketAudio(v2.data(), n, 2, frame));
    v2[12] = 0xff;
    assert(!xiaozhi::ParseWebsocketAudio(v2.data(), v2.size(), 2, frame));
    v2 = original;
    v2[3] = 1;
    assert(!xiaozhi::ParseWebsocketAudio(v2.data(), v2.size(), 2, frame));
    std::array<uint8_t, 6> v3 = {0, 0, 0, 2, 0xaa, 0xbb};
    assert(xiaozhi::ParseWebsocketAudio(v3.data(), v3.size(), 3, frame));
    assert(frame.size == 2 && frame.timestamp == 0);
    assert(!xiaozhi::ParseWebsocketAudio(v3.data(), 3, 3, frame));
    v3[3] = 3;
    assert(!xiaozhi::ParseWebsocketAudio(v3.data(), v3.size(), 3, frame));
    assert(xiaozhi::ParseWebsocketAudio(v3.data(), v3.size(), 1, frame));
    assert(!xiaozhi::ParseWebsocketAudio(v3.data(), v3.size(), 4, frame));
    assert(!xiaozhi::ParseWebsocketAudio(nullptr, 6, 1, frame));
    std::array<uint8_t, 4097> oversized{};
    assert(!xiaozhi::ParseWebsocketAudio(oversized.data(), oversized.size(), 1, frame));
    for (int rate : {8000, 12000, 16000, 24000, 48000})
        assert(xiaozhi::IsSupportedAudioParameters(rate, 60));
    assert(!xiaozhi::IsSupportedAudioParameters(44100, 60));
    assert(!xiaozhi::IsSupportedAudioParameters(16000, INT_MAX));
}
