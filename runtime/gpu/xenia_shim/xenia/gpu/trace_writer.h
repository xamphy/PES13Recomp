// Shim for Xenia's GPU trace writer: tracing is not supported, every call is a no-op.
#pragma once
#include <cstddef>
#include <cstdint>
#include "xenia/gpu/registers.h"

namespace xe::gpu
{
    class TraceWriter
    {
    public:
        bool is_open() const { return false; }
        void WriteMemoryRead(uint32_t, size_t, const void* = nullptr) {}
        void WriteMemoryReadCached(uint32_t, size_t) {}
        void WriteMemoryReadCachedNop(uint32_t, size_t) {}
        void WriteMemoryWrite(uint32_t, size_t, const void* = nullptr) {}
        void WriteEdramSnapshot(const void*) {}
    };
}
