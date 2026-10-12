#include <cassert>
#include <cstring>
#include "clock_state.h"
using namespace pocket;
int main() {
    ClockState state;
    assert(IsClockIntent("What\xe2\x80\x99s the time?"));
    assert(state.Mode() == DisplayMode::FACE);
    for (const char* command :
         {"Show me the time.", "Show time", "Open clock", "Display clock", "show the clock",
          "What's the time?", "What time is it?", "Tell me the current time",
          "Hello Pocket, please show the time!"}) {
        assert(IsClockIntent(command));
        state.UserText(command);
        assert(state.Mode() == DisplayMode::CLOCK);
    }
    for (const char* command :
         {"set a timer", "show time remaining", "open clock settings", "what time is it in Tokyo",
          "remind me to show time", "what time should I leave", "tell me a story about time",
          "set an alarm for this time", "show time and the weather"})
        assert(!IsClockIntent(command));
    // No timeout, idle/listening-state changes, lost Wi-Fi or speaker playback
    // dismisses the clock. No captured samples means no microphone transition.
    for (uint64_t now : {100ULL, 1000ULL, 3600000ULL, 86400000ULL}) {
        state.Observe(now, false, false, 0, 0);
        state.Observe(now + 1, true, true, 9000, 0);
        assert(state.Mode() == DisplayMode::CLOCK);
    }
    state.Show();
    state.Observe(89999000, true, false, 70, 90);
    state.Observe(89999700, true, false, 70, 95);
    state.Observe(90000000, true, false, 70, 100);
    state.Observe(90000250, true, false, 80, 200);
    state.Observe(90000300, true, false, 2000, 300);
    state.Observe(90000380, true, false, 70, 400);  // A short noise/click is ignored.
    assert(state.Mode() == DisplayMode::CLOCK);
    ClockState room;
    room.Show();
    for (uint32_t frame = 1; frame <= 100; ++frame) {
        room.Observe(10000 + frame * 50, true, false, frame < 4 ? 0 : 1200, frame);
        assert(room.Mode() == DisplayMode::CLOCK);  // Capture warmup and measured room noise.
    }
    room.Observe(15100, true, false, 4200, 101);
    room.Observe(15250, true, false, 4200, 102);
    assert(room.Mode() == DisplayMode::FACE);
    state.Observe(90000400, true, false, 2000, 500);
    state.Observe(90000600, true, false, 2000, 500);  // Stale samples do not trigger.
    assert(state.Mode() == DisplayMode::CLOCK);
    state.Observe(90000610, true, false, 2000, 600);
    assert(state.Mode() == DisplayMode::FACE);
    state.UserText("what is the weather today");
    state.Observe(90000700, false, true, 2000, 600);
    assert(state.Mode() == DisplayMode::FACE);
    state.UserText("what time is it");
    assert(state.Mode() == DisplayMode::CLOCK);
    // Assistant/echo energy, including its tail, cannot dismiss a clock.
    state.Observe(90000800, true, true, 12000, 700);
    state.Observe(90000900, true, false, 12000, 800);
    assert(state.Mode() == DisplayMode::CLOCK);
    for (int i = 0; i < 1000; ++i) {
        state.UserText(i % 2 ? "show time" : "tell me a joke");
        assert(state.Mode() == (i % 2 ? DisplayMode::CLOCK : DisplayMode::FACE));
    }
    std::tm time{};
    assert(!FormatClock(0, time).valid);
    time.tm_year = 126;
    time.tm_mon = 9;
    time.tm_mday = 12;
    time.tm_wday = 1;
    time.tm_hour = 22;
    time.tm_min = 28;
    auto text = FormatClock(1791844080, time);
    assert(text.valid && !std::strcmp(text.time, "10:28") && !std::strcmp(text.period, "PM"));
    assert(!std::strcmp(text.date, "Mon, 12 Oct 2026"));
    time.tm_hour = 23;
    time.tm_min = 59;
    assert(!std::strcmp(FormatClock(1791844080, time).time, "11:59"));
    time.tm_hour = 0;
    time.tm_min = 0;
    time.tm_mday = 13;
    time.tm_wday = 2;
    text = FormatClock(1791844140, time);
    assert(!std::strcmp(text.time, "12:00") && !std::strcmp(text.period, "AM"));
    assert(!std::strcmp(text.date, "Tue, 13 Oct 2026"));
    time.tm_hour = 12;
    assert(!std::strcmp(FormatClock(1791844140, time).period, "PM"));
    assert(FindClockZone("Asia/Kolkata") && !FindClockZone("invalid zone"));
}
