#include "precise_sleep.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <thread>
#include <chrono>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void PreciseSleepUs(uint32_t us)
{
    // One timer per thread, created on first use (never closed: the CP thread lives for the
    // whole session). Falls back to sleep_for if high-resolution timers are unavailable
    // (Windows before 10 1803).
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(us));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(us) * 10; // relative, 100 ns units
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(timer, INFINITE);
}
#else
#include <time.h>

void PreciseSleepUs(uint32_t us)
{
    timespec ts{ static_cast<time_t>(us / 1000000), static_cast<long>(us % 1000000) * 1000 };
    while (nanosleep(&ts, &ts) != 0) {}
}
#endif
