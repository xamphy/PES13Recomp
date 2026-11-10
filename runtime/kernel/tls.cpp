// KeTlsAlloc / KeTlsFree / KeTlsGetValue / KeTlsSetValue
//
// Slot indices are process-wide; each guest thread has its own value array
// (GuestThread::tlsSlots). The slot count comes from the XEX TLS header.
#include "kernel.h"
#include "thread.h"
#include "xex_module.h"

#include <guest_memory.h>

#include <mutex>
#include <vector>

namespace
{
    std::mutex g_tlsMutex;
    std::vector<bool> g_tlsUsed;

    uint32_t SlotAddressForCurrentThread(uint32_t index)
    {
        const Kernel::GuestThread* t = Kernel::CurrentGuestThread();
        return t ? Kernel::TlsSlotAddress(*t, index) : 0;
    }
}

GUEST_IMPORT(KeTlsAlloc)
{
    std::lock_guard lock(g_tlsMutex);
    if (g_tlsUsed.empty())
        g_tlsUsed.resize(Kernel::ExecutableModule().tlsSlotCount, false);

    for (uint32_t i = 0; i < g_tlsUsed.size(); ++i)
    {
        if (!g_tlsUsed[i])
        {
            g_tlsUsed[i] = true;
            ctx.r3.u64 = i;
            return;
        }
    }
    ctx.r3.u64 = 0xFFFFFFFF; // TLS_OUT_OF_INDEXES
}

GUEST_IMPORT(KeTlsFree)
{
    std::lock_guard lock(g_tlsMutex);
    const uint32_t index = ctx.r3.u32;
    const bool ok = index < g_tlsUsed.size() && g_tlsUsed[index];
    if (ok)
        g_tlsUsed[index] = false;
    ctx.r3.u64 = ok ? 1 : 0;
}

GUEST_IMPORT(KeTlsGetValue)
{
    const uint32_t slot = SlotAddressForCurrentThread(ctx.r3.u32);
    ctx.r3.u64 = slot ? Memory::Load32(slot) : 0;
}

GUEST_IMPORT(KeTlsSetValue)
{
    const uint32_t slot = SlotAddressForCurrentThread(ctx.r3.u32);
    if (slot)
        Memory::Store32(slot, ctx.r4.u32);
    ctx.r3.u64 = slot ? 1 : 0;
}
