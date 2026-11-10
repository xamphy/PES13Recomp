// Diagnostic overrides for the Direct3D functions that move the ring-buffer write
// position (device+11088) or CP_RB_WPTR. Every call is recorded in a small event ring;
// the GPU command processor dumps it when it sees the write pointer move backwards.
#include "kick_trace.h"

#include <ppc_context.h>
#include <kernel/thread.h>
#include <guest_memory.h>

#include <atomic>
#include <cstdio>
#include <mutex>

extern "C" PPC_FUNC(__imp__sub_822E9268);
extern "C" PPC_FUNC(__imp__sub_822E9380);
extern "C" PPC_FUNC(__imp__sub_822EA438);
extern "C" PPC_FUNC(__imp__sub_822FEA78);

namespace
{
    struct Event
    {
        uint32_t seq, thread, func, before, after, wptrBefore, wptrAfter, lr;
    };
    constexpr uint32_t EventCount = 64;
    Event g_events[EventCount];
    std::atomic<uint32_t> g_seq{ 0 };
    std::mutex g_eventMutex;

    uint32_t Device()
    {
        const uint32_t globalDevice = Memory::Load32(0x820006BC);
        return globalDevice ? Memory::Load32(globalDevice) : 0;
    }

    uint32_t Position() { const uint32_t d = Device(); return d ? Memory::Load32(d + 11088) : 0; }
    uint32_t Wptr() { return Memory::Load32(0x7FC80714); }

    template<void (*Original)(PPCContext&, uint8_t*)>
    void Traced(uint32_t func, PPCContext& ctx, uint8_t* base)
    {
        const uint64_t lr = ctx.lr;
        Kernel::GuestThread* self = Kernel::CurrentGuestThread();
        const uint32_t tid = self ? self->threadId : 0;
        const uint32_t before = Position(), wptrBefore = Wptr();
        {
            // Entry event (func | 1): shows calls that are still in flight when the dump fires.
            std::lock_guard lock(g_eventMutex);
            const uint32_t seq = g_seq++;
            g_events[seq % EventCount] = { seq, tid, func | 1, before, before, wptrBefore, wptrBefore, uint32_t(lr) };
        }
        Original(ctx, base);
        std::lock_guard lock(g_eventMutex);
        const uint32_t seq = g_seq++;
        g_events[seq % EventCount] = { seq, tid, func, before, Position(), wptrBefore, Wptr(), uint32_t(lr) };
    }
}

PPC_FUNC(sub_822E9268) { Traced<__imp__sub_822E9268>(0x822E9268, ctx, base); }
PPC_FUNC(sub_822E9380) { Traced<__imp__sub_822E9380>(0x822E9380, ctx, base); }
PPC_FUNC(sub_822EA438) { Traced<__imp__sub_822EA438>(0x822EA438, ctx, base); }
PPC_FUNC(sub_822FEA78) { Traced<__imp__sub_822FEA78>(0x822FEA78, ctx, base); }

namespace KickTrace
{
    void RecordCpRead(uint32_t rptr, uint32_t wptr)
    {
        std::lock_guard lock(g_eventMutex);
        const uint32_t seq = g_seq++;
        g_events[seq % EventCount] = { seq, 0xFFFF, 0xC0, rptr, wptr, Wptr(), Wptr(), 0 };
    }

    void Dump()
    {
        std::lock_guard lock(g_eventMutex);
        const uint32_t end = g_seq.load();
        std::fprintf(stderr, "---- ring position events (oldest first) ----\n");
        for (uint32_t s = end > 40 ? end - 40 : 0; s < end; ++s)
        {
            const Event& e = g_events[s % EventCount];
            if (e.func == 0xC0)
            {
                std::fprintf(stderr, "  #%u CP read: execute %u -> %u (WPTR now %u)\n", e.seq, e.before, e.after, e.wptrBefore);
                continue;
            }
            std::fprintf(stderr, "  #%u thread %-2u %s sub_%08X  pos %5u -> %5u  WPTR %5u -> %5u  (lr 0x%08X)\n",
                e.seq, e.thread, (e.func & 1) ? "enter" : "exit ", e.func & ~1u, e.before, e.after, e.wptrBefore, e.wptrAfter, e.lr);
        }
        std::fprintf(stderr, "----\n");
    }
}
