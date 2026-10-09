#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace pocket::face {
constexpr int kWidth = 280, kHeight = 240;
constexpr uint32_t kCream = 0xf5ecde, kPeach = 0xe8c5a9;
constexpr uint32_t kCharcoal = 0x262827, kInk = 0x191b1b;
constexpr uint32_t kCoral = 0xd58c72, kEye = 0xf9f6f0, kHighlight = 0xffffff;
constexpr uint32_t kBlush = 0xf2a0ae;
enum class Mode {
    Boot,
    Idle,
    Listening,
    Thinking,
    Speaking,
    Happy,
    Confused,
    Sleepy,
    Error,
    Setup,
    Curious,
    Shy,
    Surprised,
    Processing,
    Count
};

struct Rect {
    int x = 0, y = 0, w = 1, h = 1;
    bool operator==(const Rect& other) const {
        return x == other.x && y == other.y && w == other.w && h == other.h;
    }
    bool Inside(int margin = 0) const {
        return x >= margin && y >= margin && x + w <= kWidth - margin && y + h <= kHeight - margin;
    }
};
struct Frame {
    Rect plate{14, 30, 252, 180};
    Rect eyes[2];
    Rect mouth;
    Rect cheeks[2];
    int smile_curve = 4, smile_width = 36, head_x = 0, head_y = 0;
    uint8_t opacity = 255, plate_opacity = 255, glow = 45, mouth_opacity = 0, happy_eyes = 0;
    uint8_t dots[3] = {};
    uint8_t blush = 80;
    uint32_t eye_color = kEye;
};

inline float Ease(float value) {
    value = std::clamp(value, 0.0f, 1.0f);
    return value * value * (3.0f - 2.0f * value);
}
inline float Envelope(float current, float target, float dt) {
    // Fast attack, slower release. dt is bounded by the engine after stalls.
    const float time_constant = target > current ? 0.045f : 0.16f;
    return current + (target - current) * (1.0f - std::exp(-dt / time_constant));
}
inline float AudioTarget(uint32_t rms, float floor, float range) {
    return std::sqrt(std::clamp((static_cast<float>(rms) - floor) / range, 0.0f, 1.0f));
}

// Visual turn tracking only. It never starts/stops capture or substitutes for
// the provider's speech detection. Transcript confirmation avoids treating
// background sound alone as a completed question.
class TurnPose {
public:
    void AwaitReply(uint64_t now) {
        waiting_ = true;
        deadline_ = now + 60000;
        speech_until_ = std::max(speech_until_, now + 250);
    }
    void Observe(uint64_t now, bool listening, uint32_t mic_rms) {
        if (listening && mic_rms >= 450)
            speech_until_ = now + 900;
    }
    bool Thinking(uint64_t now) const { return waiting_ && now >= speech_until_; }
    bool TimedOut(uint64_t now) const { return waiting_ && now >= deadline_; }
    void Reset() {
        waiting_ = false;
        speech_until_ = deadline_ = 0;
    }

private:
    bool waiting_ = false;
    uint64_t speech_until_ = 0, deadline_ = 0;
};

// No ESP/LVGL dependencies, allocations, timers or application state mutations.
// A single instance is advanced by the graphics task using monotonic milliseconds.
class Engine {
public:
    explicit Engine(uint32_t seed = 0x816ad031) : random_(seed ? seed : 1) {}
    Frame Step(uint64_t now, Mode mode, uint32_t mic_rms, uint32_t playback_rms, bool compact) {
        if (!initialized_) {
            initialized_ = true;
            boot_at_ = last_at_ = mode_at_ = now;
            next_blink_ = now + 1200 + Random() % 2200;
            next_gaze_ = now + 2300;
        }
        const float dt = std::clamp(static_cast<float>(now - last_at_) / 1000.0f, 0.001f, 0.2f);
        last_at_ = now;
        if (mode != previous_mode_) {
            mode_at_ = now;
            previous_mode_ = mode;
        }
        mic_ = Envelope(mic_, mode == Mode::Listening ? AudioTarget(mic_rms, 90, 4000) : 0, dt);
        voice_ = Envelope(voice_,
                          mode == Mode::Speaking || mode == Mode::Happy
                              ? AudioTarget(playback_rms, 140, 8500)
                              : 0,
                          dt);
        if (mode == Mode::Idle && now >= next_gaze_) {
            gaze_target_ = static_cast<int>(Random() % 13) - 6;
            gaze_up_ = Random() % 4 == 0 ? -4 : 0;
            gaze_until_ = now + 900 + Random() % 1000;
            next_gaze_ = now + 3200 + Random() % 4800;
        }
        const bool glance = mode == Mode::Idle && now < gaze_until_;
        float look_x = mode == Mode::Thinking ? 2 : glance ? gaze_target_ : 0;
        float look_y = mode == Mode::Thinking ? -6 : glance ? gaze_up_ : 0;
        if (mode == Mode::Curious)
            look_x = 3;
        if (mode == Mode::Processing)
            look_x = 3 * std::sin(static_cast<float>(now - mode_at_) / 850);
        const float follow = 1 - std::exp(-dt / 0.28f);
        gaze_x_ += (look_x - gaze_x_) * follow;
        gaze_y_ += (look_y - gaze_y_) * follow;
        if (!blinking_ && now >= next_blink_) {
            blinking_ = true;
            blink_at_ = now;
            blink_duration_ = mode == Mode::Sleepy ? 460 : mode == Mode::Thinking ? 270 : 210;
        }
        float blink = 1;
        if (blinking_) {
            const float phase = static_cast<float>(now - blink_at_) / blink_duration_;
            if (phase >= 1) {
                blinking_ = false;
                if (!double_blink_ && mode != Mode::Sleepy && Random() % 6 == 0) {
                    double_blink_ = true;
                    next_blink_ = now + 130;
                } else {
                    double_blink_ = false;
                    next_blink_ = now + (mode == Mode::Thinking ? 3400 : 2400) + Random() % 4100;
                }
            } else {
                blink = 1 - Ease(phase < 0.42f ? phase / 0.42f : (1 - phase) / 0.58f);
            }
        }
        const float time = static_cast<float>(now - boot_at_) / 1000;
        const float age = static_cast<float>(now - mode_at_) / 1000;
        const float breath = std::sin(time * 1.65f);
        const float surprise = mode == Mode::Surprised ? 1 - Ease((age - 0.55f) / 0.9f) : 0;
        float desired_height = mode == Mode::Sleepy       ? 24
                               : mode == Mode::Processing ? 43
                               : mode == Mode::Shy        ? 50
                               : mode == Mode::Listening  ? 64 + 8 * mic_
                                                          : 59 + 15 * surprise + 0.8f * breath;
        float desired_width = mode == Mode::Listening ? 44 + 3 * mic_ : 43 + 3 * surprise;
        eye_height_ += (desired_height - eye_height_) * follow;
        eye_width_ += (desired_width - eye_width_) * follow;
        float bounce =
            mode == Mode::Happy ? -3.0f * std::sin(std::min(age, 1.5f) * 8) * std::exp(-age) : 0;
        const float desired_compact = compact ? -16 : 0;
        compact_y_ += (desired_compact - compact_y_) * follow;
        Frame frame;
        frame.plate.y = 30 + static_cast<int>(std::lround(compact_y_));
        frame.plate.h = 180 + static_cast<int>(std::lround(compact_y_));
        frame.head_x = static_cast<int>(std::lround(gaze_x_));
        frame.head_y = 5 + static_cast<int>(std::lround(breath * 1.4f + bounce + compact_y_));
        frame.glow =
            static_cast<uint8_t>((24 + 12 * (breath + 1) / 2 + 38 * mic_) * (0.4f + 0.6f * blink));
        happiness_ += ((mode == Mode::Happy ? 1.0f : 0.0f) - happiness_) * follow;
        frame.happy_eyes = static_cast<uint8_t>(255 * happiness_ * blink);
        float emergence = mode == Mode::Boot ? Ease(age / 1.15f) : 1;
        brightness_ += ((mode == Mode::Sleepy ? 0.62f : 1) - brightness_) * follow;
        frame.opacity = static_cast<uint8_t>(255 * emergence * brightness_);
        frame.plate_opacity = static_cast<uint8_t>(255 * emergence);
        for (int i = 0; i < 2; ++i) {
            const float target_asymmetry = mode == Mode::Confused || mode == Mode::Error ? 6
                                           : mode == Mode::Curious                       ? 5
                                                                                         : 0;
            if (i == 0)
                asymmetry_ += (target_asymmetry - asymmetry_) * follow;
            const float asymmetry = i ? -asymmetry_ : asymmetry_;
            const int w = static_cast<int>(std::lround(eye_width_));
            const int h = std::max(
                4, static_cast<int>(std::lround((eye_height_ + asymmetry) * blink * emergence)));
            const int center_y = 101 + frame.head_y + static_cast<int>(std::lround(gaze_y_));
            frame.eyes[i] = {91 + i * 98 + frame.head_x - w / 2, center_y - h / 2, w, h};
        }
        surprise_ += (surprise - surprise_) * follow;
        const float speech_opening = voice_ < 0.035f ? 0 : Ease((voice_ - 0.035f) / 0.75f);
        const float opening = std::max(speech_opening, surprise_ * 0.6f);
        const int mouth_w = static_cast<int>(std::lround(22 + 25 * speech_opening - 9 * surprise_));
        const int mouth_h = std::max(3, static_cast<int>(std::lround(3 + 28 * opening)));
        frame.mouth = {140 + frame.head_x - mouth_w / 2, 160 + frame.head_y - mouth_h / 2, mouth_w,
                       mouth_h};
        frame.mouth_opacity = static_cast<uint8_t>(255 * Ease(opening * 6));
        const float target_curve = mode == Mode::Error      ? -3
                                   : mode == Mode::Confused ? 1
                                   : mode == Mode::Happy    ? 8
                                                            : 4;
        smile_curve_ += (target_curve - smile_curve_) * follow;
        smile_width_ += ((mode == Mode::Happy ? 46 : 36) - smile_width_) * follow;
        frame.smile_curve = static_cast<int>(std::lround(smile_curve_));
        frame.smile_width = static_cast<int>(std::lround(smile_width_));
        const float target_blush = mode == Mode::Sleepy  ? 48
                                   : mode == Mode::Happy ? 145
                                   : mode == Mode::Shy   ? 165
                                                         : 85 + 30 * voice_;
        blush_ += (target_blush - blush_) * follow;
        frame.blush = static_cast<uint8_t>(blush_);
        shy_ += ((mode == Mode::Shy ? 1.0f : 0.0f) - shy_) * follow;
        for (int i = 0; i < 2; ++i) {
            const int sway = static_cast<int>(std::lround(shy_ * std::sin(time * 2.8f)));
            frame.cheeks[i] = {68 + i * 144 + frame.head_x - 20 + sway, 148 + frame.head_y - 12, 40,
                               24};
        }
        indicator_ +=
            (((mode == Mode::Thinking || mode == Mode::Setup) ? 1.0f : 0.0f) - indicator_) * follow;
        if (indicator_ > 0.005f) {
            for (int i = 0; i < 3; ++i)
                frame.dots[i] = static_cast<uint8_t>(
                    indicator_ * (30 + 100 * (0.5f + 0.5f * std::sin(time * 2 - i * 1.2f))));
        }
        return frame;
    }
    float VoiceEnvelope() const { return voice_; }

private:
    uint32_t Random() {
        random_ ^= random_ << 13;
        random_ ^= random_ >> 17;
        random_ ^= random_ << 5;
        return random_;
    }
    uint32_t random_;
    bool initialized_ = false, blinking_ = false, double_blink_ = false;
    Mode previous_mode_ = Mode::Boot;
    uint64_t boot_at_ = 0, last_at_ = 0, mode_at_ = 0, blink_at_ = 0, next_blink_ = 0,
             next_gaze_ = 0, gaze_until_ = 0, blink_duration_ = 210;
    float mic_ = 0, voice_ = 0, gaze_x_ = 0, gaze_y_ = 0, gaze_target_ = 0;
    float eye_width_ = 43, eye_height_ = 59, compact_y_ = 0;
    float happiness_ = 0, brightness_ = 1, asymmetry_ = 0, blush_ = 85, shy_ = 0;
    float smile_curve_ = 4, smile_width_ = 36, surprise_ = 0, indicator_ = 0, gaze_up_ = 0;
};
}  // namespace pocket::face
