#pragma once
#include <string>
#include "pocket_backend_policy.h"
#include "settings.h"

namespace pocket {

inline std::string BackendUrl() {
    Settings settings("pocket_backend");
    return settings.GetString("url");
}

inline std::string BackendToken() {
    Settings settings("pocket_backend");
    return settings.GetString("token");
}

inline bool BackendConfigured() {
    return ValidBackendUrl(BackendUrl().c_str()) && ValidDeviceToken(BackendToken().c_str());
}

inline bool SaveBackend(const char* url, const char* token) {
    if (!ValidBackendUrl(url) || !ValidDeviceToken(token))
        return false;
    std::string normalized = url;
    if (normalized.back() == '/')
        normalized.pop_back();
    {
        Settings settings("pocket_backend", true);
        settings.SetString("url", normalized);
        settings.SetString("token", token);
    }
    return BackendUrl() == normalized && BackendToken() == token;
}

}  // namespace pocket
