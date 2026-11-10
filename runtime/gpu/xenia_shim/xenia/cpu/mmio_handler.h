// Shim for Xenia's MMIO handler: only the types xenia/memory.h names are declared.
#pragma once
#include <cstdint>

namespace xe::cpu
{
    class MMIOHandler;
    struct MMIORange;
    using MMIOReadCallback = uint32_t (*)(void* ppc_context, void* callback_context, uint32_t addr);
    using MMIOWriteCallback = void (*)(void* ppc_context, void* callback_context, uint32_t addr, uint32_t value);
}
