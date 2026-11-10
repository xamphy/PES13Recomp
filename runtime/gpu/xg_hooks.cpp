// Hooks into the game's copy of the XG texture library.
#include "renderer.h"

#include <ppc_context.h>
#include <guest_memory.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" PPC_FUNC(__imp__sub_82B1A9D0);

// XG untile (tiled texture level -> linear buffer, 16-byte block copies). PES also uses it to
// copy resolved kit LOD atlases into their final textures (read by the GPU - no conversion
// wanted there), and in sub_82244628 on player name / number tiles, followed by an ARGB->RGBA
// byte rotation and the CPU kit compositor. Only that caller (returns to 0x822446E0) gets the
// resolve memory in the CPU byte order first (Renderer::PrepareResolveForCpu). The source
// pointer is the 9th argument, on the stack at 84(r1) ("stw r28,84(r1)" in the caller).
PPC_FUNC(sub_82B1A9D0)
{
    // OFF by default since 2026-11-08: it compensated for a wrong depth->color tile transfer
    // byte order (number tiles are cleared through a depth target at the same EDRAM base). With
    // that fixed, the standard write-back order is what the CPU expects. PES_CPU_RAW_RESOLVES=1
    // enables it (A/B).
    static const bool enabled = [] { const char* e = std::getenv("PES_CPU_RAW_RESOLVES"); return e && std::strcmp(e, "1") == 0; }();
    if (enabled && ctx.lr == 0x822446E0)
    {
        static uint32_t logs = 0;
        if (logs++ < 40)
        {
            std::printf("[xg] untile+rotate of 0x%08X (%ux%u), stack", Memory::Load32(ctx.r1.u32 + 84), ctx.r3.u32, ctx.r4.u32);
            uint32_t sp = ctx.r1.u32;
            for (int i = 0; i < 6 && sp >= 0x10000 && sp < 0x80000000u; ++i)
            {
                const uint32_t next = Memory::Load32(sp);
                if (next <= sp || next >= 0x80000000u) break;
                std::printf(" 0x%08X", Memory::Load32(next - 8));
                sp = next;
            }
            std::printf("\n");
        }
        Renderer::PrepareResolveForCpu(Memory::Load32(ctx.r1.u32 + 84));
    }
    __imp__sub_82B1A9D0(ctx, base);
}
