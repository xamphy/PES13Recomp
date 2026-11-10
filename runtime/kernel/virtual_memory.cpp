// NtAllocateVirtualMemory / NtFreeVirtualMemory / NtQueryVirtualMemory / MmQueryAddressProtect
//
// Title virtual memory lives in 0x40000000-0x6FFFFFFF (below the runtime's stack
// region). Reservations are tracked here; committing maps host pages into the
// 4 GB guest reservation, so guest pointers stay valid as plain offsets.
#include "kernel.h"
#include "thread.h"

#include <guest_memory.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <mutex>

namespace
{
    constexpr uint32_t STATUS_SUCCESS               = 0x00000000;
    constexpr uint32_t STATUS_INVALID_PARAMETER     = 0xC000000D;
    constexpr uint32_t STATUS_NO_MEMORY             = 0xC0000017;
    constexpr uint32_t STATUS_CONFLICTING_ADDRESSES = 0xC0000018;
    constexpr uint32_t STATUS_MEMORY_NOT_ALLOCATED  = 0xC00000A0;

    constexpr uint32_t MEM_COMMIT      = 0x00001000;
    constexpr uint32_t MEM_RESERVE     = 0x00002000;
    constexpr uint32_t MEM_DECOMMIT    = 0x00004000;
    constexpr uint32_t MEM_RELEASE     = 0x00008000;
    constexpr uint32_t MEM_FREE        = 0x00010000;
    constexpr uint32_t MEM_PRIVATE     = 0x00020000;
    constexpr uint32_t MEM_LARGE_PAGES = 0x20000000;
    constexpr uint32_t MEM_TOP_DOWN    = 0x40000000;

    constexpr uint32_t HeapStart = 0x40000000;
    constexpr uint32_t HeapEnd   = 0x70000000;
    constexpr uint32_t ReserveGranularity = 0x10000;

    struct Region
    {
        uint32_t size;
        uint32_t protect;
        // Committed sub-ranges, in page units of 4 KB: start -> end.
        std::map<uint32_t, uint32_t> committed;
    };

    std::mutex g_vmMutex;
    std::map<uint32_t, Region> g_regions; // va -> region

    uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    // Region containing addr, or end().
    std::map<uint32_t, Region>::iterator FindRegion(uint32_t addr)
    {
        auto it = g_regions.upper_bound(addr);
        if (it == g_regions.begin())
            return g_regions.end();
        --it;
        return addr < it->first + it->second.size ? it : g_regions.end();
    }

    bool RangeFree(uint32_t va, uint32_t size)
    {
        if (va < HeapStart || uint64_t(va) + size > HeapEnd)
            return false;
        auto it = g_regions.lower_bound(va);
        if (it != g_regions.end() && it->first < va + size)
            return false;
        if (it != g_regions.begin())
        {
            --it;
            if (it->first + it->second.size > va)
                return false;
        }
        return true;
    }

    uint32_t FindFreeRange(uint32_t size, bool topDown)
    {
        if (topDown)
        {
            uint32_t end = HeapEnd;
            for (auto it = g_regions.rbegin(); ; ++it)
            {
                const uint32_t floor = it == g_regions.rend() ? HeapStart : it->first + it->second.size;
                const uint32_t candidate = (end - size) & ~(ReserveGranularity - 1);
                if (end >= size && candidate >= floor && candidate >= HeapStart)
                    return candidate;
                if (it == g_regions.rend())
                    return 0;
                end = it->first;
            }
        }

        uint32_t cursor = HeapStart;
        for (const auto& [va, region] : g_regions)
        {
            if (va >= cursor && va - cursor >= size)
                return cursor;
            cursor = AlignUp(va + region.size, ReserveGranularity);
        }
        return uint64_t(cursor) + size <= HeapEnd ? cursor : 0;
    }

    void CommitPages(Region& region, uint32_t start, uint32_t end)
    {
        Memory::Commit(start, end - start);
        // Merge into the committed set.
        auto it = region.committed.upper_bound(start);
        if (it != region.committed.begin() && std::prev(it)->second >= start)
            --it;
        while (it != region.committed.end() && it->first <= end)
        {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = region.committed.erase(it);
        }
        region.committed[start] = end;
    }

    void DecommitPages(Region& region, uint32_t start, uint32_t end)
    {
        Memory::Decommit(start, end - start);
        auto it = region.committed.upper_bound(start);
        if (it != region.committed.begin())
            --it;
        while (it != region.committed.end() && it->first < end)
        {
            const uint32_t s = it->first, e = it->second;
            it = region.committed.erase(it);
            if (s < start) region.committed[s] = std::min(e, start);
            if (e > end) region.committed[end] = e;
        }
    }

    bool IsCommitted(const Region& region, uint32_t addr)
    {
        auto it = region.committed.upper_bound(addr);
        if (it == region.committed.begin())
            return false;
        --it;
        return addr < it->second;
    }
}

// NTSTATUS NtAllocateVirtualMemory(u32* BaseAddress, u32* RegionSize, u32 AllocationType, u32 Protect, u32 DebugMemory)
GUEST_IMPORT(NtAllocateVirtualMemory)
{
    const uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32;
    const uint32_t type = ctx.r5.u32, protect = ctx.r6.u32;
    if (!basePtr || !sizePtr || !(type & (MEM_RESERVE | MEM_COMMIT)))
    {
        ctx.r3.u64 = STATUS_INVALID_PARAMETER;
        return;
    }

    const uint32_t pageSize = (type & MEM_LARGE_PAGES) ? 0x10000 : 0x1000;
    uint32_t va = Memory::Load32(basePtr);
    uint32_t size = Memory::Load32(sizePtr);
    if (size == 0)
    {
        ctx.r3.u64 = STATUS_INVALID_PARAMETER;
        return;
    }

    std::lock_guard lock(g_vmMutex);
    uint32_t status = STATUS_SUCCESS;

    auto regionIt = va ? FindRegion(va) : g_regions.end();
    if (regionIt == g_regions.end())
    {
        // New reservation (MEM_COMMIT on unreserved memory implies MEM_RESERVE).
        const uint32_t reserveSize = AlignUp(size + (va & (ReserveGranularity - 1)), ReserveGranularity);
        uint32_t reserveBase = va & ~(ReserveGranularity - 1);
        if (reserveBase == 0)
            reserveBase = FindFreeRange(reserveSize, (type & MEM_TOP_DOWN) != 0);
        else if (!RangeFree(reserveBase, reserveSize))
            status = STATUS_CONFLICTING_ADDRESSES;

        if (status == STATUS_SUCCESS && reserveBase == 0)
            status = STATUS_NO_MEMORY;

        if (status == STATUS_SUCCESS)
        {
            regionIt = g_regions.emplace(reserveBase, Region{ reserveSize, protect, {} }).first;
            if (va == 0)
                va = reserveBase;
            if (!(type & MEM_COMMIT))
            {
                va = reserveBase;
                size = reserveSize;
            }
        }
    }

    if (status == STATUS_SUCCESS && (type & MEM_COMMIT))
    {
        Region& region = regionIt->second;
        const uint32_t start = va & ~(pageSize - 1);
        const uint32_t end = AlignUp(va + size, pageSize);
        if (end > regionIt->first + region.size)
            status = STATUS_CONFLICTING_ADDRESSES;
        else
        {
            CommitPages(region, start, end);
            va = start;
            size = end - start;
        }
    }

    if (status == STATUS_SUCCESS)
    {
        Memory::Store32(basePtr, va);
        Memory::Store32(sizePtr, size);
    }
    else
    {
        std::fprintf(stderr, "[vm] NtAllocateVirtualMemory(va=%08X size=%08X type=%08X) failed: %08X\n",
            Memory::Load32(basePtr), Memory::Load32(sizePtr), type, status);
    }
    ctx.r3.u64 = status;
}

// NTSTATUS NtFreeVirtualMemory(u32* BaseAddress, u32* RegionSize, u32 FreeType, u32 DebugMemory)
GUEST_IMPORT(NtFreeVirtualMemory)
{
    const uint32_t basePtr = ctx.r3.u32, sizePtr = ctx.r4.u32, type = ctx.r5.u32;
    if (!basePtr || !(type & (MEM_DECOMMIT | MEM_RELEASE)))
    {
        ctx.r3.u64 = STATUS_INVALID_PARAMETER;
        return;
    }

    std::lock_guard lock(g_vmMutex);
    const uint32_t va = Memory::Load32(basePtr);
    auto it = FindRegion(va);
    if (it == g_regions.end())
    {
        ctx.r3.u64 = STATUS_MEMORY_NOT_ALLOCATED;
        return;
    }

    uint32_t freedBase, freedSize;
    if (type & MEM_RELEASE)
    {
        freedBase = it->first;
        freedSize = it->second.size;
        Memory::Decommit(freedBase, freedSize);
        g_regions.erase(it);
    }
    else
    {
        uint32_t size = sizePtr ? Memory::Load32(sizePtr) : 0;
        freedBase = va & ~0xFFFu;
        const uint32_t end = size ? AlignUp(va + size, 0x1000) : it->first + it->second.size;
        freedSize = end - freedBase;
        DecommitPages(it->second, freedBase, end);
    }

    Memory::Store32(basePtr, freedBase);
    if (sizePtr)
        Memory::Store32(sizePtr, freedSize);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryVirtualMemory(u32 BaseAddress, MEMORY_BASIC_INFORMATION* Info, u32 Process?)
GUEST_IMPORT(NtQueryVirtualMemory)
{
    const uint32_t addr = ctx.r3.u32, info = ctx.r4.u32;
    std::lock_guard lock(g_vmMutex);

    // MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase, AllocationProtect,
    // RegionSize, State, Protect, Type (7 dwords).
    const uint32_t page = addr & ~0xFFFu;
    auto it = FindRegion(addr);
    if (it == g_regions.end())
    {
        Memory::Store32(info + 0x00, page);
        Memory::Store32(info + 0x04, 0);
        Memory::Store32(info + 0x08, 0);
        Memory::Store32(info + 0x0C, 0x1000);
        Memory::Store32(info + 0x10, MEM_FREE);
        Memory::Store32(info + 0x14, 0x01); // PAGE_NOACCESS
        Memory::Store32(info + 0x18, 0);
        ctx.r3.u64 = STATUS_SUCCESS;
        return;
    }

    const Region& region = it->second;
    const bool committed = IsCommitted(region, page);
    // Extend over pages with the same state.
    uint32_t end = page + 0x1000;
    while (end < it->first + region.size && IsCommitted(region, end) == committed)
        end += 0x1000;

    Memory::Store32(info + 0x00, page);
    Memory::Store32(info + 0x04, it->first);
    Memory::Store32(info + 0x08, region.protect);
    Memory::Store32(info + 0x0C, end - page);
    Memory::Store32(info + 0x10, committed ? MEM_COMMIT : MEM_RESERVE);
    Memory::Store32(info + 0x14, committed ? region.protect : 0);
    Memory::Store32(info + 0x18, MEM_PRIVATE);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
// Physical memory (GPU-visible allocations).
//
// On the console 0xA0000000/0xC0000000/0xE0000000 are three views of the same
// 512 MB of physical RAM. Here allocations come from the 0xA0000000 view only and
// the views are not aliased; MmGetPhysicalAddress strips the view bits.
// ---------------------------------------------------------------------------
namespace
{
    constexpr uint32_t PhysicalStart = 0xA0000000;
    constexpr uint32_t PhysicalEnd   = 0xC0000000;

    std::mutex g_physMutex;
    std::map<uint32_t, uint32_t> g_physAllocs; // base -> size

    uint32_t AllocPhysical(uint32_t size, uint32_t alignment, uint32_t minAddr, uint32_t maxAddr)
    {
        alignment = std::max<uint32_t>(alignment, 0x10000);
        size = AlignUp(size, 0x10000);
        uint32_t lo = std::max<uint32_t>(PhysicalStart, PhysicalStart | (minAddr & 0x1FFFFFFF));
        uint32_t hi = maxAddr ? std::min<uint64_t>(PhysicalEnd, uint64_t(PhysicalStart | (maxAddr & 0x1FFFFFFF)) + 1) : PhysicalEnd;

        std::lock_guard lock(g_physMutex);
        uint32_t cursor = AlignUp(lo, alignment);
        for (const auto& [allocBase, allocSize] : g_physAllocs)
        {
            if (allocBase >= cursor + size)
                break;
            if (allocBase + allocSize > cursor)
                cursor = AlignUp(allocBase + allocSize, alignment);
        }
        if (uint64_t(cursor) + size > hi)
            return 0;
        if (!Memory::Commit(cursor, size))
            return 0;
        g_physAllocs[cursor] = size;
        return cursor;
    }
}

namespace Kernel
{
    // Physical memory for the runtime itself (e.g. renderer scratch data), taken from the
    // top of the 512 MB so it stays clear of the title's early allocations.
    uint32_t AllocRuntimePhysical(uint32_t size)
    {
        return AllocPhysical(size, 0x10000, 0x1F000000, 0x1FFFFFFF);
    }
}

// void* MmAllocatePhysicalMemoryEx(u32 Flags, u32 Size, u32 Protect, u32 MinAddress, u32 MaxAddress, u32 Alignment)
GUEST_IMPORT(MmAllocatePhysicalMemoryEx)
{
    const uint32_t addr = AllocPhysical(ctx.r4.u32, ctx.r8.u32, ctx.r6.u32, ctx.r7.u32);
    // PES_LOG_PHYS=0x...: allocations/frees covering that physical address (with the guest caller).
    static const int64_t logPhys = [] { const char* e = std::getenv("PES_LOG_PHYS"); return e ? int64_t(std::strtoul(e, nullptr, 0)) : -1; }();
    if (logPhys >= 0 && addr && uint32_t(logPhys) >= (addr & 0x1FFFFFFFu) && uint32_t(logPhys) < (addr & 0x1FFFFFFFu) + ctx.r4.u32)
        std::printf("[vm] MmAllocatePhysicalMemoryEx -> 0x%08X size 0x%X covers 0x%08X (lr 0x%08X)\n", addr, ctx.r4.u32, uint32_t(logPhys), uint32_t(ctx.lr));
    if (logPhys >= 0 && addr && uint32_t(logPhys) >= (addr & 0x1FFFFFFFu) && uint32_t(logPhys) < (addr & 0x1FFFFFFFu) + ctx.r4.u32)
        if (Kernel::GuestThread* t = Kernel::CurrentGuestThread())
        {
            // Guest call stack (as --diag): frames linked through 0(r1), return address at back - 8.
            uint32_t frame = ctx.r1.u32;
            std::printf("[vm]   guest stack (thread %u):", t->threadId);
            for (int depth = 0; depth < 16; ++depth)
            {
                if (frame < t->stackLimit || frame >= t->stackBase) break;
                const uint32_t back = Memory::Load32(frame);
                if (back <= frame || back >= t->stackBase) break;
                std::printf(" <- 0x%08X", Memory::Load32(back - 8));
                frame = back;
            }
            std::printf("\n");
        }
    if (!addr)
        std::fprintf(stderr, "[vm] MmAllocatePhysicalMemoryEx(size=0x%X align=0x%X) failed\n", ctx.r4.u32, ctx.r8.u32);
    ctx.r3.u64 = addr;
}

// void MmFreePhysicalMemory(u32 Type, void* BaseAddress)
GUEST_IMPORT(MmFreePhysicalMemory)
{
    std::lock_guard lock(g_physMutex);
    const auto it = g_physAllocs.find(ctx.r4.u32);
    if (it != g_physAllocs.end())
    {
        static const int64_t logPhysFree = [] { const char* e = std::getenv("PES_LOG_PHYS"); return e ? int64_t(std::strtoul(e, nullptr, 0)) : -1; }();
        if (logPhysFree >= 0 && uint32_t(logPhysFree) >= (it->first & 0x1FFFFFFFu) && uint32_t(logPhysFree) < (it->first & 0x1FFFFFFFu) + it->second)
            std::printf("[vm] MmFreePhysicalMemory 0x%08X size 0x%X covers 0x%08X (lr 0x%08X)\n", it->first, it->second, uint32_t(logPhysFree), uint32_t(ctx.lr));
        Memory::Decommit(it->first, it->second);
        g_physAllocs.erase(it);
    }
}

// u32 MmGetPhysicalAddress(void* Address)
GUEST_IMPORT(MmGetPhysicalAddress)
{
    ctx.r3.u64 = ctx.r3.u32 & 0x1FFFFFFF;
}

// u32 MmQueryAddressProtect(u32 VirtualAddress)
GUEST_IMPORT(MmQueryAddressProtect)
{
    std::lock_guard lock(g_vmMutex);
    auto it = FindRegion(ctx.r3.u32);
    ctx.r3.u64 = (it != g_regions.end() && IsCommitted(it->second, ctx.r3.u32 & ~0xFFFu)) ? it->second.protect : 0;
}
