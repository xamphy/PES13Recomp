#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

// Guest (Xbox 360) address space: a single 4 GB reservation on the host.
// Guest address X lives at Memory::Base() + X. Pages are committed on demand.
//
// Guest memory is big-endian; use the Load/Store helpers for anything the game reads.
namespace Memory
{
    // Reserve the 4 GB guest space. Returns false on failure.
    bool Init();

    uint8_t* Base();

    // Commit [guestAddr, guestAddr + size) as read/write. Rounds to page size.
    bool Commit(uint32_t guestAddr, size_t size);
    // Called after every successful Commit (MemoryWatch: re-protects / invalidates watched pages).
    extern void (*g_commitHook)(uint32_t guestAddr, uint32_t size);

    // Return [guestAddr, guestAddr + size) to the reserved-but-uncommitted state
    // (contents are discarded; touching it afterwards faults).
    bool Decommit(uint32_t guestAddr, size_t size);

    template<typename T = uint8_t>
    inline T* Translate(uint32_t guestAddr) { return reinterpret_cast<T*>(Base() + guestAddr); }

    inline uint8_t  Load8 (uint32_t a) { return *Translate<uint8_t>(a); }
    inline uint16_t Load16(uint32_t a) { uint16_t v; std::memcpy(&v, Translate(a), 2); return __builtin_bswap16(v); }
    inline uint32_t Load32(uint32_t a) { uint32_t v; std::memcpy(&v, Translate(a), 4); return __builtin_bswap32(v); }
    inline uint64_t Load64(uint32_t a) { uint64_t v; std::memcpy(&v, Translate(a), 8); return __builtin_bswap64(v); }

    inline void Store8 (uint32_t a, uint8_t  v) { *Translate<uint8_t>(a) = v; }
    inline void Store16(uint32_t a, uint16_t v) { v = __builtin_bswap16(v); std::memcpy(Translate(a), &v, 2); }
    inline void Store32(uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(Translate(a), &v, 4); }
    inline void Store64(uint32_t a, uint64_t v) { v = __builtin_bswap64(v); std::memcpy(Translate(a), &v, 8); }

    // Simple bump allocators for runtime-owned guest memory. Returned memory is
    // committed and zeroed. Nothing is ever freed yet (fine for boot-time objects).
    //   Kernel: 0x80000000-0x81FFFFFF - kernel objects, module data, PCRs, TLS blocks
    //   Stacks: 0x70000000-0x7EFFFFFF - guest thread stacks
    uint32_t AllocKernel(uint32_t size, uint32_t align = 16);
    uint32_t AllocStack(uint32_t size);
}
