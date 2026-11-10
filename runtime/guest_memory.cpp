#include "guest_memory.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#   pragma comment(lib, "onecore.lib") // VirtualAlloc2 / MapViewOfFile3
#else
#   include <sys/mman.h>
#   include <unistd.h>
#   ifdef __APPLE__
#       include <mach/mach.h>
#   endif
#endif

namespace
{
    constexpr uint64_t GuestSpaceSize = 0x100000000ull; // matches PPC_MEMORY_SIZE
    constexpr uint64_t PhysicalStart = 0xA0000000ull;    // first physical-memory view
    constexpr uint64_t PhysicalViewSize = 0x20000000ull; // 512 MB
    constexpr size_t PageSize = 0x1000;
    uint8_t* g_base = nullptr;

    struct BumpRegion
    {
        const char* name;
        uint32_t start;
        uint32_t end;
        uint32_t cursor;
        std::mutex mutex;

        // Reserves [addr, addr + size) but commits only [addr + uncommittedHead, addr + size).
        uint32_t Alloc(uint32_t size, uint32_t align, uint32_t uncommittedHead = 0)
        {
            std::lock_guard lock(mutex);
            const uint32_t addr = (cursor + align - 1) & ~(align - 1);
            if (size == 0 || uncommittedHead >= size || uint64_t(addr) + size > end)
            {
                std::fprintf(stderr, "[memory] %s region exhausted (wanted 0x%X bytes)\n", name, size);
                return 0;
            }
            const uint32_t usable = addr + uncommittedHead;
            const uint32_t usableSize = size - uncommittedHead;
            if (!Memory::Commit(usable, usableSize))
            {
                std::fprintf(stderr, "[memory] commit failed at 0x%08X (0x%X bytes)\n", usable, usableSize);
                return 0;
            }
            cursor = addr + size;
            std::memset(Memory::Translate(usable), 0, usableSize); // a page may be shared with an earlier allocation
            return addr;
        }
    };

#ifndef _WIN32
    // Committed 4 KB guest pages (host pages can be larger: 16 KB on Apple arm64), so Decommit
    // releases a host page only when none of its guest pages is still committed.
    std::mutex g_commitMutex;
    uint64_t g_committed[GuestSpaceSize / PageSize / 64];
    bool IsCommitted(uint64_t page) { return (g_committed[page >> 6] >> (page & 63)) & 1; }
#endif

    BumpRegion g_kernelRegion{ "kernel", 0x80000000, 0x82000000, 0x80000000 };
    BumpRegion g_stackRegion{ "stack", 0x70000000, 0x7F000000, 0x70000000 };
}

namespace Memory
{
    bool Init()
    {
#ifdef _WIN32
        // Layout of the 4 GB reservation:
        //   0x00000000-0x9FFFFFFF  ordinary reserved memory (virtual heap, stacks, MMIO, kernel, image)
        //   0xA0000000-0xBFFFFFFF  \
        //   0xC0000000-0xDFFFFFFF   > three views of ONE 512 MB section = console physical RAM
        //   0xE0000000-0xFFFFFFFF  /
        // Built with placeholders so the section views can replace parts of the reservation.
        // TODO: the console's 0xE0000000 view is offset by 0x1000 physical bytes; mapped at 0 here.
        auto* base = static_cast<uint8_t*>(VirtualAlloc2(nullptr, nullptr, GuestSpaceSize,
            MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0));
        bool ok = base != nullptr;
        ok = ok && VirtualFree(base, PhysicalStart, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        ok = ok && VirtualFree(base + 0xA0000000ull, PhysicalViewSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        ok = ok && VirtualFree(base + 0xC0000000ull, PhysicalViewSize, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        ok = ok && VirtualAlloc2(nullptr, base, PhysicalStart, MEM_RESERVE | MEM_REPLACE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        HANDLE section = ok ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_RESERVE, 0,
            DWORD(PhysicalViewSize), nullptr) : nullptr;
        ok = ok && section;
        for (uint64_t view : { 0xA0000000ull, 0xC0000000ull, 0xE0000000ull })
            ok = ok && MapViewOfFile3(section, nullptr, base + view, 0, PhysicalViewSize,
                MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (!ok)
            std::fprintf(stderr, "[memory] guest address space setup failed (error %lu)\n", GetLastError());
        g_base = ok ? base : nullptr;
#else
        void* p = mmap(nullptr, GuestSpaceSize, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        g_base = p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#   ifdef __APPLE__
        // Same layout as Windows: the three physical views share ONE 512 MB block. vm_allocate
        // (zero-fill on demand, so untouched pages cost nothing) at the first view, then vm_remap
        // with copy = false aliases it at the other two. Works on macOS and iPadOS (no shm_open there).
        if (g_base)
        {
            const mach_port_t task = mach_task_self();
            vm_address_t first = vm_address_t(g_base + PhysicalStart);
            kern_return_t kr = vm_allocate(task, &first, PhysicalViewSize, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE);
            for (uint64_t view : { 0xC0000000ull, 0xE0000000ull })
            {
                if (kr != KERN_SUCCESS) break;
                vm_address_t target = vm_address_t(g_base + view);
                vm_prot_t cur = 0, max = 0;
                kr = vm_remap(task, &target, PhysicalViewSize, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                    task, first, FALSE, &cur, &max, VM_INHERIT_NONE);
            }
            if (kr != KERN_SUCCESS)
            {
                std::fprintf(stderr, "[memory] physical view setup failed (kern_return %d)\n", int(kr));
                munmap(g_base, GuestSpaceSize);
                g_base = nullptr;
            }
        }
#   endif
#endif
        if (!g_base)
        {
            std::fprintf(stderr, "[memory] failed to reserve 4 GB guest address space\n");
            return false;
        }
        return true;
    }

    uint8_t* Base() { return g_base; }
    void (*g_commitHook)(uint32_t guestAddr, uint32_t size) = nullptr;

    bool Commit(uint32_t guestAddr, size_t size)
    {
        const uint64_t start = guestAddr & ~uint64_t(PageSize - 1);
        const uint64_t end = (uint64_t(guestAddr) + size + PageSize - 1) & ~uint64_t(PageSize - 1);
        if (end > GuestSpaceSize)
            return false;
#ifdef _WIN32
        const bool ok = VirtualAlloc(g_base + start, end - start, MEM_COMMIT, PAGE_READWRITE) != nullptr;
        if (ok && g_commitHook) g_commitHook(uint32_t(start), uint32_t(end - start));
        return ok;
#else
        // mprotect needs host-page alignment (16 KB on Apple arm64): commit the enclosing host pages.
        // The physical views are always read/write already (mapped as one block in Init).
        bool ok = true;
        if (start < PhysicalStart)
        {
            const uint64_t hostPage = uint64_t(getpagesize());
            const uint64_t hStart = start & ~(hostPage - 1);
            const uint64_t hEnd = (end + hostPage - 1) & ~(hostPage - 1);
            std::lock_guard lock(g_commitMutex);
            ok = mprotect(g_base + hStart, hEnd - hStart, PROT_READ | PROT_WRITE) == 0;
            if (ok)
                for (uint64_t page = start / PageSize; page < end / PageSize; ++page)
                    g_committed[page >> 6] |= 1ull << (page & 63);
        }
        if (ok && g_commitHook) g_commitHook(uint32_t(start), uint32_t(end - start));
        return ok;
#endif
    }

    bool Decommit(uint32_t guestAddr, size_t size)
    {
        const uint64_t start = guestAddr & ~uint64_t(PageSize - 1);
        const uint64_t end = (uint64_t(guestAddr) + size + PageSize - 1) & ~uint64_t(PageSize - 1);
        if (end > GuestSpaceSize)
            return false;
#ifdef _WIN32
        if (start >= PhysicalStart)
        {
            // Section views can't be decommitted page by page; just clear the contents.
            std::memset(g_base + start, 0, size_t(end - start));
            return true;
        }
        return VirtualFree(g_base + start, end - start, MEM_DECOMMIT) != 0;
#else
        // Physical views: clear only (remapping would break the aliasing), as on Windows.
        if (start >= PhysicalStart)
        {
            std::memset(g_base + start, 0, size_t(end - start));
            return true;
        }
        // Elsewhere: forget the guest pages; a host page (16 KB on Apple arm64) is released once none
        // of its guest pages is committed, otherwise the decommitted guest pages are zeroed so a later
        // Commit sees zeros (Windows semantics).
        const uint64_t hostPage = uint64_t(getpagesize());
        const uint64_t perHost = hostPage / PageSize;
        std::lock_guard lock(g_commitMutex);
        for (uint64_t page = start / PageSize; page < end / PageSize; ++page)
            if (IsCommitted(page))
            {
                std::memset(g_base + page * PageSize, 0, PageSize);
                g_committed[page >> 6] &= ~(1ull << (page & 63));
            }
        bool ok = true;
        for (uint64_t h = start & ~(hostPage - 1); h < end; h += hostPage)
        {
            bool used = false;
            for (uint64_t page = h / PageSize; page < h / PageSize + perHost; ++page)
                used = used || IsCommitted(page);
            if (!used)
                ok = mmap(g_base + h, hostPage, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != MAP_FAILED && ok;
        }
        return ok;
#endif
    }

    uint32_t AllocKernel(uint32_t size, uint32_t align)
    {
        return g_kernelRegion.Alloc(size, align < 4 ? 4 : align);
    }

    uint32_t AllocStack(uint32_t size)
    {
        // A 64 KB gap below every stack: an overflow runs into uncommitted memory
        // and crashes immediately instead of corrupting the neighbouring stack.
        const uint32_t guard = 0x10000;
        size = (size + 0xFFFF) & ~0xFFFFu;
        const uint32_t block = g_stackRegion.Alloc(size + guard, 0x10000, guard);
        return block ? block + guard : 0;
    }
}
