// Fallback implementations for every xboxkrnl/xam import listed in imports.inc.
//
// They are weak definitions: implementing an import for real is just a matter of
// writing GUEST_IMPORT(Name) { ... } in another file - the strong definition wins
// at link time (requires lld-link, which the build uses).
#include "kernel.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace Kernel
{
    void Unimplemented(const char* library, const char* name, PPCContext& ctx)
    {
        static std::mutex mutex;
        static std::unordered_map<std::string, uint32_t> counts;

        uint32_t count;
        {
            std::lock_guard lock(mutex);
            count = ++counts[name];
        }

        if (count <= 3)
        {
            std::fprintf(stderr, "[stub] %s!%s(r3=%08X r4=%08X r5=%08X r6=%08X) lr=%08llX%s\n",
                library, name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                (unsigned long long)ctx.lr, count == 3 ? "  (further calls not logged)" : "");
        }

        ctx.r3.u64 = 0;
    }
}

#define IMPORT(library, name) \
    __attribute__((weak)) GUEST_IMPORT(name) { Kernel::Unimplemented(#library, #name, ctx); }
#include "imports.inc"
#undef IMPORT
