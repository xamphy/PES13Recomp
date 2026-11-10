// Debug: catch every store to the page holding CP_RB_WPTR (guest 0x7FC80714) using
// page protection + single-step, and report stores that move WPTR backwards together
// with the host instruction (resolve with tools\addr2func.ps1).
#include "write_watch.h"

#include <guest_memory.h>
#include <kernel/thread.h>

#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdio>

namespace
{
    constexpr uint32_t WatchedGuestAddr = 0x7FC80714; // CP_RB_WPTR
    uint8_t* g_page = nullptr;
    std::atomic<uint32_t> g_lastValue{ 0 };
    std::atomic<uint32_t> g_reports{ 0 };
    std::atomic<uint32_t> g_stores{ 0 };
    uint32_t g_recent[8] = {};

    // Second watch (debug): first store that puts '%' at g_byteAddr.
    uint8_t* g_bytePage = nullptr;
    uint32_t g_byteAddr = 0;
    std::atomic<uint32_t> g_byteReports{ 0 };

    // Third watch (debug, PES_WATCH_PHYS=0x...): first store to a guest physical page through
    // any of the three physical views (0xA0000000 / 0xC0000000 / 0xE0000000).
    uint8_t* g_physPages[3] = {};
    uint32_t g_physAddr = 0;
    uint32_t g_watchFrom = 0; // only bytes from this page offset on count (virtual watch of a buffer)
    uint8_t* g_readPage = nullptr; // WatchFirstRead
    uint32_t g_readGuest = 0;
    std::atomic<uint32_t> g_physReports{ 0 };

    struct Pending
    {
        bool active = false;
        uintptr_t rip = 0;
        uintptr_t addr = 0;
        uint8_t* page = nullptr;
    };
    thread_local Pending t_pending;

    uint32_t ReadWatched() { return Memory::Load32(WatchedGuestAddr); }

    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        auto* rec = info->ExceptionRecord;
        // One-shot read watch (WatchFirstRead): report the first access, then unprotect.
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && g_readPage)
        {
            const auto addr = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
            if (addr >= uintptr_t(g_readPage) && addr < uintptr_t(g_readPage) + 0x1000)
            {
                const auto module = uintptr_t(GetModuleHandleW(nullptr));
                Kernel::GuestThread* self = Kernel::CurrentGuestThread();
                std::fprintf(stderr, "[watch] first %s of guest 0x%08X (+0x%llX) by rva 0x%llX (guest thread %u, lr 0x%08X)\n",
                    rec->ExceptionInformation[0] == 1 ? "write" : "read", g_readGuest, (unsigned long long)(addr - uintptr_t(g_readPage)),
                    (unsigned long long)(uintptr_t(rec->ExceptionAddress) - module), self ? self->threadId : 0,
                    self && self->context ? uint32_t(self->context->lr) : 0);
                DWORD old;
                VirtualProtect(g_readPage, 0x1000, PAGE_READWRITE, &old);
                g_readPage = nullptr;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && rec->ExceptionInformation[0] == 1)
        {
            const auto addr = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
            for (uint8_t* p : g_physPages)
                if (p && addr >= uintptr_t(p) && addr < uintptr_t(p) + 0x1000)
                {
                    // Let the store happen (single-step), then check whether it left non-zero data.
                    for (uint8_t* q : g_physPages)
                    {
                        DWORD old;
                        if (q) VirtualProtect(q, 0x1000, PAGE_READWRITE, &old);
                    }
                    t_pending = { true, uintptr_t(rec->ExceptionAddress), addr, p };
                    info->ContextRecord->EFlags |= 0x100;
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
            uint8_t* page = nullptr;
            if (g_page && addr >= uintptr_t(g_page) && addr < uintptr_t(g_page) + 0x1000) page = g_page;
            else if (g_bytePage && addr >= uintptr_t(g_bytePage) && addr < uintptr_t(g_bytePage) + 0x1000) page = g_bytePage;
            if (page)
            {
                DWORD old;
                VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
                t_pending = { true, uintptr_t(rec->ExceptionAddress), addr, page };
                info->ContextRecord->EFlags |= 0x100; // single-step the store
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        else if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_pending.active)
        {
            t_pending.active = false;
            bool physPage = false;
            for (uint8_t* p : g_physPages) physPage |= p && t_pending.page == p;
            if (physPage)
            {
                bool nonZero = false;
                for (uint32_t i = g_watchFrom; i < 0x1000 && !nonZero; ++i) nonZero = t_pending.page[i] != 0;
                if (nonZero)
                {
                    if (g_physReports++ == 0)
                    {
                        const auto module = uintptr_t(GetModuleHandleW(nullptr));
                        Kernel::GuestThread* self = Kernel::CurrentGuestThread();
                        std::fprintf(stderr, "[watch] first non-zero store to physical 0x%08X (+0x%llX) by rva 0x%llX (guest thread %u, lr 0x%08X, host thread %lu)\n",
                            g_physAddr, (unsigned long long)(t_pending.addr - uintptr_t(t_pending.page)), (unsigned long long)(t_pending.rip - module),
                            self ? self->threadId : 0, self && self->context ? uint32_t(self->context->lr) : 0, GetCurrentThreadId());
                    }
                    return EXCEPTION_CONTINUE_EXECUTION; // stop watching
                }
                for (uint8_t* q : g_physPages)
                {
                    DWORD old;
                    if (q) VirtualProtect(q, 0x1000, PAGE_READONLY, &old);
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (t_pending.page == g_bytePage)
            {
                if (Memory::Load8(g_byteAddr) == '%' && g_byteReports++ == 0)
                {
                    const auto module = uintptr_t(GetModuleHandleW(nullptr));
                    Kernel::GuestThread* self = Kernel::CurrentGuestThread();
                    std::fprintf(stderr, "[watch] '%%' stored at 0x%08X by rva 0x%llX (guest thread %u, lr 0x%08X)\n", g_byteAddr,
                        (unsigned long long)(t_pending.rip - module), self ? self->threadId : 0,
                        self && self->context ? uint32_t(self->context->lr) : 0);
                }
                DWORD old;
                VirtualProtect(g_bytePage, 0x1000, g_byteReports ? PAGE_READWRITE : PAGE_READONLY, &old);
                return EXCEPTION_CONTINUE_EXECUTION;
            }
            if (t_pending.addr == uintptr_t(Memory::Translate(WatchedGuestAddr)))
            {
                const uint32_t value = ReadWatched();
                const uint32_t previous = g_lastValue.exchange(value);
                g_recent[g_stores++ % 8] = value;
                const uint32_t back = (previous - value) & 0x1FFF; // ring is 8192 dwords
                if (back != 0 && back < 64 && g_reports++ < 10)
                {
                    const auto module = uintptr_t(GetModuleHandleW(nullptr));
                    std::fprintf(stderr, "[watch] WPTR %u -> %u written at rva 0x%llX (host thread %lu)\n", previous, value,
                        (unsigned long long)(t_pending.rip - module), GetCurrentThreadId());
                }
            }
            DWORD old;
            VirtualProtect(g_page, 0x1000, PAGE_READONLY, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace WriteWatch
{
    void WatchFirstPercent(uint32_t guestAddr)
    {
        static bool installed = false;
        if (g_bytePage) return;
        g_byteAddr = guestAddr;
        g_bytePage = Memory::Translate(guestAddr & ~0xFFFu);
        if (!installed && !g_page) AddVectoredExceptionHandler(1, Handler);
        installed = true;
        DWORD old;
        VirtualProtect(g_bytePage, 0x1000, PAGE_READONLY, &old);
        std::fprintf(stderr, "[watch] watching 0x%08X for a '%%'\n", guestAddr);
    }

    void WatchPhysicalPage(uint32_t physAddr)
    {
        static bool installed = false;
        g_physAddr = physAddr & 0x1FFFF000u;
        const uint32_t views[3] = { 0xA0000000u, 0xC0000000u, 0xE0000000u };
        for (int i = 0; i < 3; ++i)
        {
            g_physPages[i] = Memory::Translate(views[i] | g_physAddr);
            DWORD old;
            VirtualProtect(g_physPages[i], 0x1000, PAGE_READONLY, &old);
        }
        if (!installed) AddVectoredExceptionHandler(1, Handler);
        installed = true;
        std::fprintf(stderr, "[watch] watching stores to physical page 0x%08X (all views)\n", g_physAddr);
    }

    void PrintRecent()
    {
        const uint32_t n = g_stores.load();
        std::fprintf(stderr, "[watch] %u WPTR stores seen; most recent:", n);
        for (uint32_t i = n > 8 ? n - 8 : 0; i < n; ++i) std::fprintf(stderr, " %u", g_recent[i % 8]);
        std::fprintf(stderr, "  (register now %u)\n", ReadWatched());
    }

    void EnableWptrWatch()
    {
        g_page = Memory::Translate(WatchedGuestAddr & ~0xFFFu);
        g_lastValue = ReadWatched();
        AddVectoredExceptionHandler(1, Handler);
        DWORD old;
        VirtualProtect(g_page, 0x1000, PAGE_READONLY, &old);
        std::printf("[watch] watching stores to CP_RB_WPTR\n");
    }
}

namespace WriteWatch
{
    // Same report as WatchPhysicalPage, for one guest virtual page (first non-zero store only).
    void WatchVirtualPage(uint32_t guestAddr)
    {
        if (g_physPages[0] || g_physPages[1] || g_physPages[2]) return; // one watch at a time
        g_physAddr = guestAddr & ~0xFFFu;
        g_watchFrom = guestAddr & 0xFFFu;
        g_physPages[0] = Memory::Translate(g_physAddr);
        DWORD old;
        VirtualProtect(g_physPages[0], 0x1000, PAGE_READONLY, &old);
        static bool installed = false;
        if (!installed) AddVectoredExceptionHandler(1, Handler);
        installed = true;
        std::fprintf(stderr, "[watch] watching stores to guest page 0x%08X\n", g_physAddr);
    }
}

namespace WriteWatch
{
    // Report the first access (read or write) to the guest page holding guestAddr, once.
    void WatchFirstRead(uint32_t guestAddr)
    {
        if (g_readPage) return;
        g_readGuest = guestAddr;
        uint8_t* page = Memory::Translate(guestAddr & ~0xFFFu);
        static bool installed = false;
        if (!installed) AddVectoredExceptionHandler(1, Handler);
        installed = true;
        g_readPage = page;
        DWORD old;
        VirtualProtect(page, 0x1000, PAGE_NOACCESS, &old);
        std::fprintf(stderr, "[watch] watching the first access to guest 0x%08X\n", guestAddr);
    }
}

namespace
{
    // WatchStores: report the first few stores (any value) to one guest page.
    uint8_t* g_storePage = nullptr;
    uint32_t g_storeGuest = 0;
    std::atomic<int> g_storeReportsLeft{ 0 };
    LONG CALLBACK StoreHandler(EXCEPTION_POINTERS* info)
    {
        auto* rec = info->ExceptionRecord;
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && g_storePage &&
            rec->ExceptionInformation[0] == 1)
        {
            const auto addr = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
            if (addr >= uintptr_t(g_storePage) && addr < uintptr_t(g_storePage) + 0x1000)
            {
                const auto module = uintptr_t(GetModuleHandleW(nullptr));
                Kernel::GuestThread* self = Kernel::CurrentGuestThread();
                std::fprintf(stderr, "[watch] store to guest 0x%08X (+0x%llX) by rva 0x%llX (guest thread %u, lr 0x%08X)\n",
                    g_storeGuest, (unsigned long long)(addr - uintptr_t(g_storePage)),
                    (unsigned long long)(uintptr_t(rec->ExceptionAddress) - module), self ? self->threadId : 0,
                    self && self->context ? uint32_t(self->context->lr) : 0);
                DWORD old;
                VirtualProtect(g_storePage, 0x1000, PAGE_READWRITE, &old);
                // Watch again after this store (single-step), until the report budget is used up.
                if (--g_storeReportsLeft > 0) { info->ContextRecord->EFlags |= 0x100; }
                else g_storePage = nullptr;
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
        if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && g_storePage && g_storeReportsLeft > 0)
        {
            DWORD old;
            VirtualProtect(g_storePage, 0x1000, PAGE_READONLY, &old);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace WriteWatch
{
    void WatchStores(uint32_t guestAddr, int reports)
    {
        if (g_storePage) return;
        static bool installed = false;
        if (!installed) AddVectoredExceptionHandler(1, StoreHandler);
        installed = true;
        g_storeGuest = guestAddr;
        g_storeReportsLeft = reports;
        g_storePage = Memory::Translate(guestAddr & ~0xFFFu);
        DWORD old;
        VirtualProtect(g_storePage, 0x1000, PAGE_READONLY, &old);
        std::fprintf(stderr, "[watch] watching stores to guest page of 0x%08X\n", guestAddr);
    }
}
