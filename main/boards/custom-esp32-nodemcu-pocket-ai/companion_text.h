#pragma once
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace pocket {
template <size_t N>
void CopyUtf8(char (&dest)[N], const char* source) {
    if (!source)
        source = "";
    size_t length = std::min(std::strlen(source), N - 1);
    if (source[length]) {
        while (length && (static_cast<unsigned char>(source[length]) & 0xc0) == 0x80)
            --length;
    }
    std::memcpy(dest, source, length);
    dest[length] = 0;
}
inline bool TechnicalMessage(const char* text) {
    if (!text)
        return false;
    for (const auto* token :
         {"POCKET", "Pocket", "XIAOZHI", "xiaozhi/", "ESP32", "IDF", "Heap", "heap",
          "Hardware diagnostics", "Offline:", "Ver ", "firmware", "I2S", "diag over serial"})
        if (std::strstr(text, token))
            return true;
    return false;
}
}  // namespace pocket
