// Direct3D's wait-loop helper sub_822EA8F8 (called once per iteration of every D3D wait loop:
// BlockOnFence, swap throttle, ring space, resource Lock). On the console it burns a few cycles
// (32x db16cyc) and runs the GPU hang detector; returns 1 = keep waiting, 0 = give up.
// Recompiled, the loops spin at full speed: the game's main thread burned ~1 core waiting in
// Swap -> BlockOnFence. Here: the first SpinUs of a wait stay a pure spin (short waits keep
// their latency), after that every iteration sleeps SleepUs with a high-resolution timer.
#include "d3d_wait.h"
#include "precise_sleep.h"

#include <ppc_context.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

extern "C" PPC_FUNC(__imp__sub_822EA8F8);

namespace
{
    constexpr int64_t SpinUs = 100;
    constexpr uint32_t SleepUs = 200;
    // Calls further apart than this belong to different waits.
    constexpr int64_t SameWaitGapUs = 1000;

    std::atomic<bool> g_spin{ [] { const char* v = std::getenv("PES_D3D_SPIN"); return v && *v && *v != '0'; }() };

    int64_t NowUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
}

namespace D3DWait
{
    void SetSpin(bool spin)
    {
        g_spin = spin;
        std::printf("[d3d] wait loops: %s\n", spin ? "spin" : "spin+sleep");
    }
}

PPC_FUNC(sub_822EA8F8)
{
    __imp__sub_822EA8F8(ctx, base);
    if (ctx.r3.u32 != 1 || g_spin.load(std::memory_order_relaxed))
        return;

    thread_local int64_t waitStart = 0, lastCall = 0;
    const int64_t now = NowUs();
    if (now - lastCall > SameWaitGapUs)
        waitStart = now;
    if (now - waitStart >= SpinUs)
        PreciseSleepUs(SleepUs);
    lastCall = NowUs();
}
