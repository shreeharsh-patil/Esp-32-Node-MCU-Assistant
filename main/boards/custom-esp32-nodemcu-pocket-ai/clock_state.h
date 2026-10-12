#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string_view>

namespace pocket {
enum class DisplayMode { FACE, CLOCK };

// Intentionally narrow: mentioning time in a timer, alarm, story or compound
// request must not open the clock. Transcription punctuation/case are harmless.
inline bool IsClockIntent(std::string_view text) {
    char normalized[128] = {};
    size_t size = 0;
    bool space = false;
    for (size_t index = 0; index < text.size(); ++index) {
        const unsigned char c = text[index];
        if (c == 0xe2 && index + 2 < text.size() &&
            static_cast<unsigned char>(text[index + 1]) == 0x80 &&
            (static_cast<unsigned char>(text[index + 2]) == 0x98 ||
             static_cast<unsigned char>(text[index + 2]) == 0x99)) {
            index += 2;  // Typographic apostrophes in provider transcripts.
            continue;
        }
        if (c == '\'' || c == '"' || c == '?' || c == '.' || c == '!' || c == ',')
            continue;
        if (std::isspace(c)) {
            space = size != 0;
            continue;
        }
        if (space && size < sizeof(normalized) - 1)
            normalized[size++] = ' ';
        space = false;
        if (size >= sizeof(normalized) - 1 || c >= 128)
            return false;
        normalized[size++] = static_cast<char>(std::tolower(c));
    }
    std::string_view command(normalized, size);
    for (auto prefix : {std::string_view("hello pocket "), std::string_view("hey pocket "),
                        std::string_view("pocket "), std::string_view("please ")}) {
        if (command.substr(0, prefix.size()) == prefix)
            command.remove_prefix(prefix.size());
    }
    if (command.size() > 7 && command.substr(command.size() - 7) == " please")
        command.remove_suffix(7);
    constexpr std::string_view commands[] = {"show me the time",
                                             "show me time",
                                             "show time",
                                             "show the time",
                                             "open clock",
                                             "open the clock",
                                             "display clock",
                                             "display the clock",
                                             "show clock",
                                             "show the clock",
                                             "whats the time",
                                             "what is the time",
                                             "what time is it",
                                             "tell me the current time",
                                             "tell me the time",
                                             "what is the current time",
                                             "whats the current time"};
    for (auto allowed : commands)
        if (command == allowed)
            return true;
    return false;
}

struct ClockText {
    bool valid = false;
    char time[8] = "--:--", period[3] = {}, date[32] = "Synchronizing time...";
};
inline ClockText FormatClock(std::time_t epoch, const std::tm& local) {
    ClockText result;
    // A cold ESP32 RTC starts near 1970. Both discovery UTC and SNTP can
    // establish valid time; no invented fallback date is displayed.
    if (epoch < 1704067200 || local.tm_year < 124 || local.tm_year > 199 || local.tm_mon < 0 ||
        local.tm_mon > 11 || local.tm_wday < 0 || local.tm_wday > 6)
        return result;
    constexpr const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    constexpr const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    result.valid = true;
    std::snprintf(result.time, sizeof(result.time), "%d:%02d",
                  local.tm_hour % 12 ? local.tm_hour % 12 : 12, local.tm_min);
    std::snprintf(result.period, sizeof(result.period), "%s", local.tm_hour < 12 ? "AM" : "PM");
    std::snprintf(result.date, sizeof(result.date), "%s, %d %s %d", days[local.tm_wday],
                  local.tm_mday, months[local.tm_mon], local.tm_year + 1900);
    return result;
}

class ClockState {
public:
    DisplayMode Mode() const { return mode_; }
    void Show() {
        mode_ = DisplayMode::CLOCK;
        armed_ = false;
        quiet_since_ = loud_since_ = 0;
    }
    void Face() { mode_ = DisplayMode::FACE; }
    void UserText(std::string_view text) {
        if (text.empty())
            return;
        if (IsClockIntent(text))
            Show();
        else
            Face();
    }
    // LiteAudioEngine has no local VAD. Reuse actual captured RMS with a
    // sustained-energy gate, quiet arming and playback tail suppression.
    // Transcribed user text remains the definitive fallback. Never sample a
    // stale capture block, or mistake listening-state entry for user speech.
    void Observe(uint64_t now, bool listening, bool playback, uint32_t rms, uint32_t frames) {
        if (playback || !listening) {
            was_listening_ = false;
            quiet_since_ = loud_since_ = 0;
            if (playback) {
                blocked_until_ = now + 500;
                armed_ = false;
            }
            last_frames_ = frames;
            return;
        }
        if (frames == last_frames_ || now < blocked_until_)
            return;
        last_frames_ = frames;
        if (!was_listening_) {
            was_listening_ = true;
            learn_until_ = now + 650;
            armed_ = false;
        }
        // Capture starts with near-zero/stale RMS before the INMP441 settles.
        // Learn the real room level first, otherwise that startup ramp could
        // dismiss a clock without any user speech (observed on the device).
        if (now < learn_until_) {
            noise_ = (noise_ * 3 + std::min<uint32_t>(rms, 5000)) / 4;
            quiet_since_ = loud_since_ = 0;
            return;
        }
        const uint32_t threshold = std::max<uint32_t>(450, noise_ * 3 + 100);
        if (rms < threshold) {
            noise_ = (noise_ * 31 + std::min<uint32_t>(rms, 1500)) / 32;
            loud_since_ = 0;
            if (!quiet_since_)
                quiet_since_ = now;
            if (now - quiet_since_ >= 200)
                armed_ = true;
        } else {
            quiet_since_ = 0;
            if (!loud_since_)
                loud_since_ = now;
            if (armed_ && now - loud_since_ >= 100) {
                Face();
                armed_ = false;
            }
        }
    }

private:
    DisplayMode mode_ = DisplayMode::FACE;
    bool armed_ = false, was_listening_ = false;
    uint32_t noise_ = 80, last_frames_ = 0;
    uint64_t quiet_since_ = 0, loud_since_ = 0, blocked_until_ = 0, learn_until_ = 0;
};

struct ClockZone {
    const char* name;
    const char* posix;
};
// ESP-IDF/newlib has POSIX timezone rules, not an IANA database. A small
// explicit mapping gives settings and the provider the same unambiguous zone.
inline constexpr ClockZone kClockZones[] = {{"Asia/Kolkata", "IST-5:30"},
                                            {"Etc/UTC", "UTC0"},
                                            {"Asia/Dubai", "GST-4"},
                                            {"Asia/Singapore", "SGT-8"},
                                            {"Asia/Tokyo", "JST-9"},
                                            {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0/2"},
                                            {"America/New_York", "EST5EDT,M3.2.0/2,M11.1.0/2"},
                                            {"America/Los_Angeles", "PST8PDT,M3.2.0/2,M11.1.0/2"}};
inline const ClockZone* FindClockZone(std::string_view name) {
    for (const auto& zone : kClockZones)
        if (name == zone.name)
            return &zone;
    return nullptr;
}
}  // namespace pocket
