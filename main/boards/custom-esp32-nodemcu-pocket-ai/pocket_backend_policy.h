#pragma once
#include <cstring>

namespace pocket {

inline bool ValidBackendUrl(const char* url) {
    if (!url || std::strlen(url) > 192 || std::strncmp(url, "https://", 8) != 0)
        return false;
    const char* host = url + 8;
    size_t length = std::strlen(host);
    if (length && host[length - 1] == '/')
        --length;
    if (!length || length > 128)
        return false;
    size_t label = 0;
    bool dot = false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = host[i];
        if (ch == '.') {
            if (!label || host[i - 1] == '-')
                return false;
            label = 0;
            dot = true;
        } else {
            const bool alpha_numeric =
                (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
            if ((!alpha_numeric && ch != '-') || (!label && ch == '-') || ++label > 63)
                return false;
        }
    }
    return dot && label && host[length - 1] != '-';
}

inline bool ValidDeviceToken(const char* token) {
    if (!token)
        return false;
    const size_t length = std::strlen(token);
    if (length < 32 || length > 128)
        return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = token[i];
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == '_' || ch == '-'))
            return false;
    }
    return true;
}

}  // namespace pocket
