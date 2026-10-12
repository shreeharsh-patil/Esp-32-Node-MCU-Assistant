#pragma once
#include <string>

namespace pocket {
std::string ClockTimezone();
void LoadClockTimezone();
bool SaveClockTimezone(const char* name);
// Called on the existing Wi-Fi worker on each connected edge; never waits
// for DNS, a response or synchronization on the application/graphics task.
void StartClockSync();
}  // namespace pocket
