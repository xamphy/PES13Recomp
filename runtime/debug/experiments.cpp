// Experimental behaviour switches (debugging only, never on by default).
#include "experiments.h"

#include <ppc_context.h>
#include <guest_memory.h>
#include <kernel/thread.h>

#include <atomic>
#include <cstdio>

extern "C" PPC_FUNC(__imp__sub_822E9F18);
extern "C" PPC_FUNC(__imp__sub_822FA5D8);

namespace Experiments
{
    bool g_skipFenceWait = false;
    bool g_noVsyncWait = false;
}

// Direct3D BlockOnFence(device, fence, ...): with --skip-fence-wait, return at once
// instead of waiting for the GPU to pass the fence. Incorrect (the CPU may touch
// resources the GPU is still using), but shows whether the game progresses.
PPC_FUNC(sub_822E9F18)
{
    if (Experiments::g_skipFenceWait)
        return;
    __imp__sub_822E9F18(ctx, base);
}

// Direct3D GPU-hang handler, called by the wait-loop helper sub_822EA8F8 after ~5000
// ticks without fence progress. A non-zero result makes every wait give up.
PPC_FUNC(sub_822FA5D8)
{
    static std::atomic<uint32_t> calls{ 0 };
    const uint32_t device = uint32_t(ctx.r3.u32);
    const uint32_t n = ++calls;
    __imp__sub_822FA5D8(ctx, base);
    if (n <= 10 || (n & (n - 1)) == 0)
    {
        Kernel::GuestThread* self = Kernel::CurrentGuestThread();
        std::fprintf(stderr, "[d3d] GPU hang handler #%u on thread %u -> %u (device flags +11069 = 0x%02X)\n",
            n, self ? self->threadId : 0, ctx.r3.u32, Memory::Load8(device + 11069));
    }
}
