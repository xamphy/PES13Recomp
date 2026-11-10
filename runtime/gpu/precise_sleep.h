#pragma once
#include <cstdint>

// Sleeps for about `us` microseconds with sub-millisecond precision (std::this_thread::sleep_for
// on Windows rounds up to the system timer tick, often 1-15.6 ms). Windows: a per-thread
// high-resolution waitable timer; other platforms: nanosleep, which is already precise.
void PreciseSleepUs(uint32_t us);
