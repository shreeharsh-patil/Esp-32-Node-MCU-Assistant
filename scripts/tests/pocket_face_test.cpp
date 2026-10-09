#include <cassert>
#include <cstring>
#include "companion_text.h"
#include "face_animation.h"

using namespace pocket::face;
int main() {
    Engine boot(1);
    assert(boot.Step(0, Mode::Boot, 0, 0, false).opacity == 0);
    assert(boot.Step(1300, Mode::Boot, 0, 0, false).opacity == 255);
    Engine speaking(2);
    uint64_t now = 0;
    Frame frame;
    for (int i = 0; i < 30; ++i, now += 33)
        frame = speaking.Step(now, Mode::Speaking, 0, 9000, false);
    assert(frame.mouth.h > 20 && frame.mouth_opacity > 200);
    // A service Speaking state with silent PCM must close the mouth.
    for (int i = 0; i < 60; ++i, now += 33)
        frame = speaking.Step(now, Mode::Speaking, 0, 0, false);
    assert(frame.mouth_opacity == 0 && frame.mouth.h == 3);
    assert(speaking.VoiceEnvelope() < 0.001f);
    // Low background noise does not drive speech animation.
    for (int i = 0; i < 30; ++i, now += 33)
        frame = speaking.Step(now, Mode::Speaking, 0, 100, false);
    assert(frame.mouth_opacity == 0);
    Engine silent(3), listening(3);
    Frame quiet, loud;
    for (int i = 0; i < 12; ++i) {
        quiet = silent.Step(i * 33, Mode::Listening, 0, 0, false);
        loud = listening.Step(i * 33, Mode::Listening, 5000, 0, false);
    }
    assert(loud.eyes[0].h > quiet.eyes[0].h && loud.glow > quiet.glow);
    TurnPose turn;
    turn.Observe(1000, true, 5000);
    assert(!turn.Thinking(3000));  // Noise alone cannot confirm a question.
    turn.AwaitReply(3000);
    turn.Observe(3100, true, 2000);
    assert(!turn.Thinking(3999) && turn.Thinking(4000));
    turn.Observe(4100, true, 2000);
    assert(!turn.Thinking(4200) && turn.Thinking(5000));
    assert(!turn.TimedOut(62999) && turn.TimedOut(63000));
    turn.Reset();
    assert(!turn.Thinking(70000) && !turn.TimedOut(70000));
    Engine neutral(3), thinker(3), happy(3), shy(3), sleepy(3);
    Frame normal, up, joyful, bashful, dim;
    for (int i = 0; i < 30; ++i) {
        normal = neutral.Step(i * 33, Mode::Idle, 0, 0, false);
        up = thinker.Step(i * 33, Mode::Thinking, 0, 0, false);
        joyful = happy.Step(i * 33, Mode::Happy, 0, 0, false);
        bashful = shy.Step(i * 33, Mode::Shy, 0, 0, false);
        dim = sleepy.Step(i * 33, Mode::Sleepy, 0, 0, false);
    }
    assert(up.eyes[0].y + up.eyes[0].h / 2 < normal.eyes[0].y + normal.eyes[0].h / 2 - 3);
    assert(joyful.blush > normal.blush && bashful.blush > joyful.blush);
    assert(dim.blush < normal.blush && dim.opacity < normal.opacity);
    assert(joyful.smile_width > normal.smile_width);
    Engine animated(23);
    int blink_count = 0, double_count = 0;
    uint64_t previous_blink = 0;
    bool closed = false;
    for (uint64_t time = 0; time < 120000; time += 16) {
        const auto f = animated.Step(time, Mode::Idle, 0, 0, false);
        if (f.eyes[0].h <= 8 && !closed) {
            ++blink_count;
            if (previous_blink && time - previous_blink < 700)
                ++double_count;
            previous_blink = time;
        }
        closed = f.eyes[0].h <= 8;
    }
    assert(blink_count > 15 && blink_count < 65 && double_count > 0);
    Engine talking(23);
    bool saw_talking_blink = false;
    for (uint64_t time = 0; time < 10000; time += 16) {
        const auto f = talking.Step(time, Mode::Speaking, 0, 9000, false);
        if (time > 1000 && f.eyes[0].h <= 8) {
            saw_talking_blink = true;
            assert(f.mouth.h > 20);  // Mouth and blinking run independently.
        }
    }
    assert(saw_talking_blink);
    Engine endurance(4);
    // Three simulated hours, varied state changes, compact captions, frame stalls,
    // full-scale input, every expression, and both silent/active playback.
    for (uint64_t time = 0; time < 10800000; time += 33) {
        const auto mode = static_cast<Mode>((time / 2700) % static_cast<int>(Mode::Count));
        const uint64_t stamp = time + (time / 3000) * 200;
        auto f = endurance.Step(stamp, mode, time % 6000 < 2500 ? 32768 : 0,
                                time % 3000 < 1800 ? 32768 : 0, (time / 4100) % 2);
        assert(f.plate.Inside(5));
        assert(f.mouth.Inside(8));
        for (const auto& cheek : f.cheeks) {
            assert(cheek.Inside(8));
            assert(cheek.y + cheek.h < 185);
        }
        assert(f.cheeks[1].x - f.cheeks[0].x == 144);
        for (const auto& eye : f.eyes) {
            assert(eye.Inside(8));
            assert(Rect({eye.x - 7, eye.y - 7, eye.w + 14, eye.h + 14}).Inside());
            assert(eye.x > f.plate.x && eye.x + eye.w < f.plate.x + f.plate.w);
        }
    }
    char small[7];
    pocket::CopyUtf8(small, "hello\xe2\x82\xac");
    assert(std::strcmp(small, "hello") == 0);
    pocket::CopyUtf8(small, nullptr);
    assert(small[0] == 0);
    assert(pocket::TechnicalMessage("ESP32 / IDF 6.1 / Heap 120000"));
    assert(pocket::TechnicalMessage("POCKET / XIAOZHI"));
    assert(pocket::TechnicalMessage("Ver 2.5.1"));
    assert(!pocket::TechnicalMessage("Hotspot: Xiaozhi-1234 Config URL: http://192.168.4.1"));
}
