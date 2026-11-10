// Deferred procedure calls. Direct3D queues DPCs from its graphics interrupt
// handler; they run on a dedicated guest thread, in queue order.
//
// X_KDPC: u16 Type, u8 SelectedCpu, u8 DesiredCpu, LIST_ENTRY DpcListEntry (+0x04),
//         DeferredRoutine (+0x0C), DeferredContext (+0x10), SystemArgument1/2 (+0x14/+0x18)
#include "kernel.h"
#include "thread.h"
#include "interrupts.h"

#include <guest_memory.h>

#include <atomic>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace
{
    std::mutex g_dpcMutex;
    std::condition_variable g_dpcCv;
    std::deque<uint32_t> g_dpcQueue;
    std::atomic<bool> g_dpcThreadStarted{ false };

    void DpcThread()
    {
        Kernel::GuestThread* thread = Kernel::CreateGuestThread(0x10000);
        if (!thread)
            return;
        for (;;)
        {
            uint32_t dpc;
            {
                std::unique_lock lock(g_dpcMutex);
                g_dpcCv.wait(lock, [] { return !g_dpcQueue.empty(); });
                dpc = g_dpcQueue.front();
                g_dpcQueue.pop_front();
                Memory::Store32(dpc + 0x04, 0); // no longer queued
            }
            const uint32_t routine = Memory::Load32(dpc + 0x0C);
            if (routine)
            {
                // A DPC runs on the CPU it was queued on and preempts that CPU's thread.
                const uint32_t cpu = Memory::Load8(dpc + 0x02) % Kernel::CpuCount;
                Memory::Store8(thread->pcr + 0x10C, uint8_t(cpu));
                static int ran = 0;
                if (ran++ < 6) std::printf("[dpc] run 0x%08X on CPU %u\n", dpc, cpu);
                std::lock_guard cpuLock(Kernel::CpuInterruptLock(cpu));
                Kernel::CallGuestFunction(*thread, routine, dpc, Memory::Load32(dpc + 0x10),
                    Memory::Load32(dpc + 0x14), Memory::Load32(dpc + 0x18));
            }
        }
    }
}

// void KeInitializeDpc(KDPC* Dpc, void* DeferredRoutine, void* DeferredContext)
GUEST_IMPORT(KeInitializeDpc)
{
    const uint32_t dpc = ctx.r3.u32;
    Memory::Store16(dpc + 0x00, 19); // DpcObject
    Memory::Store8(dpc + 0x02, 0);
    Memory::Store8(dpc + 0x03, 0);
    Memory::Store32(dpc + 0x04, 0);
    Memory::Store32(dpc + 0x08, 0);
    Memory::Store32(dpc + 0x0C, ctx.r4.u32);
    Memory::Store32(dpc + 0x10, ctx.r5.u32);
    Memory::Store32(dpc + 0x14, 0);
    Memory::Store32(dpc + 0x18, 0);
}

// BOOLEAN KeInsertQueueDpc(KDPC* Dpc, void* SystemArgument1, void* SystemArgument2)
GUEST_IMPORT(KeInsertQueueDpc)
{
    const uint32_t dpc = ctx.r3.u32;
    bool inserted = false;
    {
        std::lock_guard lock(g_dpcMutex);
        if (Memory::Load32(dpc + 0x04) == 0) // not already queued
        {
            Memory::Store32(dpc + 0x04, dpc); // non-zero marks "queued"
            Memory::Store8(dpc + 0x02, uint8_t(Kernel::CurrentCpu())); // SelectedCpu: run where queued
            Memory::Store32(dpc + 0x14, ctx.r4.u32);
            Memory::Store32(dpc + 0x18, ctx.r5.u32);
            g_dpcQueue.push_back(dpc);
            inserted = true;
            static int logged = 0;
            if (logged++ < 6)
                std::printf("[dpc] queue 0x%08X routine 0x%08X from CPU %u (bytes +2=%02X +3=%02X)\n", dpc, Memory::Load32(dpc + 0x0C),
                    Kernel::CurrentCpu(), Memory::Load8(dpc + 0x02), Memory::Load8(dpc + 0x03));
        }
    }
    if (inserted)
    {
        if (!g_dpcThreadStarted.exchange(true))
            std::thread(DpcThread).detach();
        g_dpcCv.notify_one();
    }
    static int notInserted = 0;
    if (!inserted && notInserted++ < 3)
        std::printf("[dpc] 0x%08X already queued - not inserted\n", dpc);
    ctx.r3.u64 = inserted ? 1 : 0;
}
