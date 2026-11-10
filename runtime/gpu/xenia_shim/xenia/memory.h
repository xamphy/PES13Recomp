// Shim for Xenia's Memory: the GPU helpers only translate physical addresses. The runtime
// sets membase to the host address of guest physical address 0.
#pragma once
#include <cstdint>
#include "xenia/base/memory.h"

namespace xe
{
    class Memory
    {
    public:
        using GlobalWatchHandle = void*;
        using WatchHandle = void*;
        static constexpr uint32_t kBufferSize = 512u << 20; // guest physical memory

        uint8_t* membase = nullptr;
        uint8_t* physical_membase() const { return membase; }

        template <typename T = uint8_t*>
        T TranslatePhysical(uint32_t guest_address) const
        {
            return reinterpret_cast<T>(membase + (guest_address & 0x1FFFFFFF));
        }
    };
}
