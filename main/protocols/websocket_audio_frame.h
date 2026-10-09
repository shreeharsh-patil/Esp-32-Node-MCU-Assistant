#pragma once
#include <cstddef>
#include <cstdint>

// Exact upstream BinaryProtocol2 (16 bytes) and BinaryProtocol3 (4 bytes),
// read without alignment assumptions or writing into the network callback buffer.
namespace xiaozhi {
struct WebsocketAudioFrame {
    const uint8_t* payload = nullptr;
    size_t size = 0;
    uint32_t timestamp = 0;
};
inline uint32_t ReadBigEndian32(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
           bytes[3];
}
inline bool ParseWebsocketAudio(const uint8_t* bytes, size_t length, int version,
                                WebsocketAudioFrame& frame) {
    frame = {};
    if (!bytes || length == 0 || length > 4112)
        return false;
    size_t offset = 0;
    size_t payload_size = length;
    if (version == 2) {
        if (length < 16 || bytes[0] != 0 || bytes[1] != 2 || bytes[2] != 0 || bytes[3] != 0)
            return false;
        offset = 16;
        payload_size = ReadBigEndian32(bytes + 12);
        frame.timestamp = ReadBigEndian32(bytes + 8);
    } else if (version == 3) {
        if (length < 4 || bytes[0] != 0)
            return false;
        offset = 4;
        payload_size = (size_t(bytes[2]) << 8) | bytes[3];
    } else if (version != 1)
        return false;
    if (payload_size == 0 || payload_size > 4096 || payload_size != length - offset)
        return false;
    frame.payload = bytes + offset;
    frame.size = payload_size;
    return true;
}
inline bool IsSupportedAudioParameters(int sample_rate, int duration) {
    const bool rate = sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 ||
                      sample_rate == 24000 || sample_rate == 48000;
    const bool frame = duration == 5 || duration == 10 || duration == 20 || duration == 40 ||
                       duration == 60 || duration == 80 || duration == 100 || duration == 120;
    return rate && frame;
}
}  // namespace xiaozhi
