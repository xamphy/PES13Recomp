#include "interrupts.h"
#include "thread.h"

#include <guest_memory.h>

namespace
{
    std::recursive_mutex g_cpuLocks[Kernel::CpuCount];
}

namespace Kernel
{
    std::recursive_mutex& CpuInterruptLock(uint32_t cpu)
    {
        return g_cpuLocks[cpu % CpuCount];
    }

    uint32_t CurrentCpu()
    {
        GuestThread* t = CurrentGuestThread();
        return t ? Memory::Load8(t->pcr + 0x10C) % CpuCount : 0;
    }
}

// Called by recompiled `mtmsrd` when MSR[EE] changes.
void ppcOnMsrEEChange(PPCContext& ctx, uint32_t oldMsr)
{
    Kernel::GuestThread* t = Kernel::CurrentGuestThread();
    if (!t)
        return;

    const bool wasEnabled = (oldMsr & 0x8000) != 0;
    const bool nowEnabled = (ctx.msr & 0x8000) != 0;
    if (wasEnabled && !nowEnabled)
    {
        const uint32_t cpu = Kernel::CurrentCpu();
        Kernel::CpuInterruptLock(cpu).lock();
        t->heldCpuLock = int32_t(cpu);
    }
    else if (!wasEnabled && nowEnabled && t->heldCpuLock >= 0)
    {
        Kernel::CpuInterruptLock(uint32_t(t->heldCpuLock)).unlock();
        t->heldCpuLock = -1;
    }
}
