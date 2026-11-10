#pragma once
#include <ppc_context.h>

namespace Kernel
{
    // Called by every import that has no real implementation yet.
    // Logs the call (first few times per import) and returns 0 in r3.
    void Unimplemented(const char* library, const char* name, PPCContext& ctx);

    // Guest physical memory (0xA0000000 view address) reserved for the runtime; 0 on failure.
    uint32_t AllocRuntimePhysical(uint32_t size);
}

// Declare/define a guest import with the same signature the recompiled code expects.
#define GUEST_IMPORT(name) PPC_FUNC(__imp__##name)
