#include "memory_watch.h"

#include <guest_memory.h>
#include <kernel/thread.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#else
#   include <csignal>
#   include <dlfcn.h>
#   include <sys/mman.h>
#   include <unistd.h>
#   if defined(__APPLE__)
#       include <mach-o/dyld.h>
#   endif
#endif

#include <atomic>
#include <memory>
#include <mutex>

namespace
{
    // Tracking granularity = host page size: 4 KB on Windows/x64, 16 KB on Apple Silicon
    // (mprotect works on whole host pages).
#if defined(__APPLE__) && defined(__aarch64__)
    constexpr uint32_t kPageShift = 14;
#else
    constexpr uint32_t kPageShift = 12;
#endif
    constexpr uint32_t kPageSize = 1u << kPageShift;
    constexpr uint32_t kPhysicalSize = 0x20000000;            // 512 MB, one view
    constexpr uint32_t kPages = kPhysicalSize >> kPageShift;
    constexpr uint32_t kViews[3] = { 0xA0000000, 0xC0000000, 0xE0000000 };

    // ------------------------------------------------------------------ platform layer
    enum class Prot { ReadWrite, ReadOnly, NoAccess };

    bool HostProtect(void* p, size_t size, Prot prot)
    {
#ifdef _WIN32
        const DWORD w = prot == Prot::ReadWrite ? PAGE_READWRITE : prot == Prot::ReadOnly ? PAGE_READONLY : PAGE_NOACCESS;
        DWORD old;
        return VirtualProtect(p, size, w, &old) != 0;
#else
        const int m = prot == Prot::ReadWrite ? (PROT_READ | PROT_WRITE) : prot == Prot::ReadOnly ? PROT_READ : PROT_NONE;
        return mprotect(p, size, m) == 0;
#endif
    }

    // Base of the main executable, for "host rva" in reports.
    uintptr_t HostModuleBase()
    {
#ifdef _WIN32
        return uintptr_t(GetModuleHandleW(nullptr));
#elif defined(__APPLE__)
        return uintptr_t(_dyld_get_image_header(0));
#else
        return 0;
#endif
    }

    std::unique_ptr<std::atomic<uint8_t>[]> g_protected;      // page is write-protected (all views)
    std::unique_ptr<std::atomic<uint32_t>[]> g_lastWrite;     // epoch of the last recorded write
    std::atomic<uint32_t> g_epoch{ 1 };
    std::atomic<uint64_t> g_faults{ 0 }; // recorded writes (each one a fault + 3 protect calls)
    std::mutex g_protectMutex;                                 // protect calls + flag updates
    uintptr_t g_viewBase[3] = {};

    void SetPages(uint32_t firstPage, uint32_t count, Prot protection)
    {
        for (uintptr_t view : g_viewBase)
            HostProtect(reinterpret_cast<void*>(view + (uintptr_t(firstPage) << kPageShift)), size_t(count) << kPageShift, protection);
    }

    // Debug (ReportNextWrite): pages whose next write is reported with the writing guest code.
    std::unique_ptr<std::atomic<uint8_t>[]> g_report;
    uint32_t g_reportFirst = 0, g_reportCount = 0, g_reportsLeft = 0;

    void PrintGuestBackChain(Kernel::GuestThread* self)
    {
        // Guest back chain: 0(r1) -> caller frame; the prologue saved LR at -8 of the caller's frame.
        uint32_t sp = self && self->context ? self->context->r1.u32 : 0;
        for (int i = 0; i < 10 && sp >= 0x10000 && sp < 0x80000000u; ++i)
        {
            const uint32_t next = Memory::Load32(sp);
            if (next <= sp || next >= 0x80000000u) break;
            std::printf(" 0x%08X", Memory::Load32(next - 8));
            sp = next;
        }
        std::printf("\n");
    }

    // Records a write to a protected page and unprotects it. Returns false if the page was not
    // protected (the fault is someone else's).
    thread_local uintptr_t t_faultRip = 0; // host instruction of the current fault (report)
    thread_local bool t_rearmAccess = false;  // ReportAccess: re-protect after a single-stepped host access
    bool RecordWrite(uint32_t page)
    {
        if (g_report && g_report[page].load())
        {
            std::lock_guard lock(g_protectMutex);
            Kernel::GuestThread* self = Kernel::CurrentGuestThread();
            if (!self) { if (g_report[page].exchange(0) && !g_protected[page].load()) { SetPages(page, 1, Prot::ReadWrite); return true; } }
            else if (g_report[page].exchange(0))
            {
                static std::set<uint32_t> seenLr; // one line per writing location
                const uint32_t lr = self->context ? uint32_t(self->context->lr) : 0;
                const bool fresh = seenLr.insert(uint32_t(t_faultRip) * 31u + (page >> 8)).second; // per writer and region
                if (fresh)
                {
                    std::printf("[memwatch] report: write to physical 0x%08X by guest thread %u, lr 0x%08X, host rva 0x%llX, r3 0x%08X r4 0x%08X r5 0x%08X r29 0x%08X, stack", page << kPageShift,
                        self->threadId, lr, (unsigned long long)(t_faultRip ? t_faultRip - HostModuleBase() : 0),
                        self->context ? self->context->r3.u32 : 0, self->context ? self->context->r4.u32 : 0,
                        self->context ? self->context->r5.u32 : 0, self->context ? self->context->r29.u32 : 0);
                    PrintGuestBackChain(self);
                }
                if (fresh && --g_reportsLeft == 0)
                    for (uint32_t p = g_reportFirst; p < g_reportFirst + g_reportCount; ++p)
                        if (g_report[p].exchange(0) && !g_protected[p].load()) SetPages(p, 1, Prot::ReadWrite);
                if (!g_protected[page].load()) { SetPages(page, 1, Prot::ReadWrite); return true; }
            }
        }
        std::lock_guard lock(g_protectMutex);
        if (!g_protected[page].load()) return false;
        g_protected[page] = 0;
        g_lastWrite[page] = g_epoch.fetch_add(1) + 1;
        ++g_faults;
        SetPages(page, 1, Prot::ReadWrite);
        return true;
    }

    // Memory::Commit of physical memory: fresh contents (the old surfaces there are stale) and
    // the committing view's protection was reset to read/write - watched pages are re-protected.
    void OnCommit(uint32_t guestAddr, uint32_t size)
    {
        if (guestAddr < kViews[0] || !size) return;
        const uint32_t physical = guestAddr & (kPhysicalSize - 1);
        const uint32_t first = physical >> kPageShift;
        const uint32_t last = std::min<uint32_t>((uint64_t(physical) + size - 1) >> kPageShift, kPages - 1);
        for (uint32_t p = first; p <= last; ++p)
        {
            if (g_protected[p].load()) RecordWrite(p);
            if (g_report && g_report[p].load()) SetPages(p, 1, Prot::ReadOnly);
        }
    }

    // Debug (ReportVirtualWrites): a virtual (heap) guest range whose writers are reported.
    uint32_t g_vBase = 0, g_vSize = 0, g_vReportsLeft = 0;
    uint32_t g_accessBase = 0, g_accessSize = 0; // ReportAccess (debug)
    std::vector<uint8_t> g_vArmed;
    std::mutex g_vMutex;

    bool ReportVirtual(uintptr_t addr)
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(Memory::Translate(g_vBase));
        if (!g_vSize || addr < start || addr >= start + g_vSize) return false;
        std::lock_guard lock(g_vMutex);
        const uint32_t page = uint32_t((addr - start) >> kPageShift);
        if (!g_vArmed[page]) return false;
        g_vArmed[page] = 0;
        HostProtect(reinterpret_cast<void*>(start + (uintptr_t(page) << kPageShift)), kPageSize, Prot::ReadWrite);
        Kernel::GuestThread* self = Kernel::CurrentGuestThread();
        static std::set<uint32_t> seen;
        if (!self || !self->context || !g_vReportsLeft) return true;
        const uint32_t guestAddr = g_vBase + (page << kPageShift);
        if (!seen.insert(uint32_t(t_faultRip) * 31u + (guestAddr >> 20)).second) return true;
        --g_vReportsLeft;
        std::printf("[memwatch] vreport: write to 0x%08X by guest thread %u, host rva 0x%llX, r3 0x%08X r4 0x%08X r5 0x%08X, stack", guestAddr,
            self->threadId, (unsigned long long)(t_faultRip - HostModuleBase()), self->context->r3.u32,
            self->context->r4.u32, self->context->r5.u32);
        PrintGuestBackChain(self);
        return true;
    }

    // Debug access watch (ReportAccess) hit by guest code: print, then restore the pages.
    void ReportAccessHit(uintptr_t a, uintptr_t view, bool write, uintptr_t pc)
    {
        Kernel::GuestThread* self = Kernel::CurrentGuestThread();
        {
            // The first bytes as the guest sees them (pages are not readable yet: use the host view).
            HostProtect(reinterpret_cast<void*>(g_viewBase[0] + g_accessBase), kPageSize, Prot::ReadOnly);
            const uint8_t* b = reinterpret_cast<const uint8_t*>(g_viewBase[0] + g_accessBase);
            std::printf("[memwatch] bytes at 0x%08X:", g_accessBase);
            for (int i = 0; i < 32; ++i) std::printf("%s%02X", (i % 4) ? "" : " ", b[i]);
            std::printf("\n");
        }
        std::printf("[memwatch] access: %s of physical 0x%08X by guest thread %u, host rva 0x%llX, r3 0x%08X r4 0x%08X r5 0x%08X, stack",
            write ? "write" : "read", uint32_t(a - view), self ? self->threadId : 0,
            (unsigned long long)(pc - HostModuleBase()),
            self && self->context ? self->context->r3.u32 : 0, self && self->context ? self->context->r4.u32 : 0,
            self && self->context ? self->context->r5.u32 : 0);
        PrintGuestBackChain(self);
        // One report: restore the pages (read-only where the surface watch wants them).
        for (uintptr_t v : g_viewBase)
            HostProtect(reinterpret_cast<void*>(v + g_accessBase), g_accessSize, Prot::ReadWrite);
        for (uint32_t p = g_accessBase >> kPageShift; p < (g_accessBase + g_accessSize) >> kPageShift; ++p)
            if (g_protected[p].load()) SetPages(p, 1, Prot::ReadOnly);
        g_accessSize = 0;
    }

    // Write fault at host address `addr` (not an access-watch hit). True = handled, retry.
    bool HandleWriteFault(uintptr_t addr, uintptr_t pc)
    {
        t_faultRip = pc;
        if (ReportVirtual(addr)) return true;
        for (uintptr_t view : g_viewBase)
            if (addr >= view && addr < view + kPhysicalSize)
                return RecordWrite(uint32_t((addr - view) >> kPageShift));
        return false;
    }

#ifdef _WIN32
    LONG CALLBACK Handler(EXCEPTION_POINTERS* info)
    {
        const auto* rec = info->ExceptionRecord;
        if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_rearmAccess)
        {
            t_rearmAccess = false;
            if (g_accessSize)
                for (uintptr_t v : g_viewBase)
                    HostProtect(reinterpret_cast<void*>(v + g_accessBase), g_accessSize, Prot::NoAccess);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
        // Debug access watch (ReportAccess): any read or write of the watched physical pages.
        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && g_accessSize)
        {
            const uintptr_t a = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
            for (uintptr_t view : g_viewBase)
                if (a >= view + g_accessBase && a < view + g_accessBase + g_accessSize)
                {
                    std::lock_guard lock(g_protectMutex);
                    if (!Kernel::CurrentGuestThread())
                    {
                        // Host code (the renderer): let this one access through, re-arm after it.
                        for (uintptr_t v : g_viewBase)
                            HostProtect(reinterpret_cast<void*>(v + g_accessBase), g_accessSize, Prot::ReadWrite);
                        t_rearmAccess = true;
                        info->ContextRecord->EFlags |= 0x100; // single step
                        return EXCEPTION_CONTINUE_EXECUTION;
                    }
                    ReportAccessHit(a, view, rec->ExceptionInformation[0] == 1, uintptr_t(rec->ExceptionAddress));
                    return EXCEPTION_CONTINUE_EXECUTION;
                }
        }
        if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2 || rec->ExceptionInformation[0] != 1)
            return EXCEPTION_CONTINUE_SEARCH;
        return HandleWriteFault(static_cast<uintptr_t>(rec->ExceptionInformation[1]), uintptr_t(rec->ExceptionAddress))
            ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
#else
    struct sigaction g_prevSegv{}, g_prevBus{};

    // Write bit of the faulting access, and the faulting pc, from the signal context.
    bool FaultIsWrite(void* uc, uintptr_t& pc)
    {
        auto* ctx = static_cast<ucontext_t*>(uc);
#if defined(__APPLE__) && defined(__aarch64__)
        pc = uintptr_t(ctx->uc_mcontext->__ss.__pc);
        return (ctx->uc_mcontext->__es.__esr & (1u << 6)) != 0;   // ESR_ELx.WnR
#elif defined(__APPLE__) && defined(__x86_64__)
        pc = uintptr_t(ctx->uc_mcontext->__ss.__rip);
        return (ctx->uc_mcontext->__es.__err & 2) != 0;           // page-fault error code: W
#elif defined(__linux__) && defined(__aarch64__)
        pc = uintptr_t(ctx->uc_mcontext.pc);
        return true; // ESR is in a reserved-space record; treat as a write (only protected pages are handled)
#elif defined(__linux__) && defined(__x86_64__)
        pc = uintptr_t(ctx->uc_mcontext.gregs[REG_RIP]);
        return (ctx->uc_mcontext.gregs[REG_ERR] & 2) != 0;
#else
        pc = 0;
        return true;
#endif
    }

    void Chain(const struct sigaction& prev, int sig, siginfo_t* info, void* uc)
    {
        if (prev.sa_flags & SA_SIGINFO)
        {
            if (prev.sa_sigaction) { prev.sa_sigaction(sig, info, uc); return; }
        }
        else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler)
        {
            prev.sa_handler(sig);
            return;
        }
        // Default action: restore it and return; the faulting instruction re-faults and crashes.
        signal(sig, SIG_DFL);
    }

    void SignalHandler(int sig, siginfo_t* info, void* uc)
    {
        const uintptr_t a = uintptr_t(info->si_addr);
        uintptr_t pc = 0;
        const bool write = FaultIsWrite(uc, pc);
        if (g_accessSize)
            for (uintptr_t view : g_viewBase)
                if (a >= view + g_accessBase && a < view + g_accessBase + g_accessSize)
                {
                    std::lock_guard lock(g_protectMutex);
                    if (Kernel::CurrentGuestThread())
                        ReportAccessHit(a, view, write, pc);
                    else
                    {
                        // No single-step here: a host access just ends the watch.
                        std::printf("[memwatch] access watch ended by a host access (no single-step on this platform)\n");
                        for (uintptr_t v : g_viewBase)
                            HostProtect(reinterpret_cast<void*>(v + g_accessBase), g_accessSize, Prot::ReadWrite);
                        for (uint32_t p = g_accessBase >> kPageShift; p < (g_accessBase + g_accessSize) >> kPageShift; ++p)
                            if (g_protected[p].load()) SetPages(p, 1, Prot::ReadOnly);
                        g_accessSize = 0;
                    }
                    return;
                }
        if (write && HandleWriteFault(a, pc)) return;
        Chain(sig == SIGBUS ? g_prevBus : g_prevSegv, sig, info, uc);
    }
#endif
}

namespace MemoryWatch
{
    void Init()
    {
        if (g_protected) return;
#ifndef _WIN32
        if (sysconf(_SC_PAGESIZE) > long(kPageSize))
            std::fprintf(stderr, "[memwatch] host page size %ld > tracking page size %u - tracking will misbehave\n", sysconf(_SC_PAGESIZE), kPageSize);
#endif
        g_protected = std::make_unique<std::atomic<uint8_t>[]>(kPages);
        g_lastWrite = std::make_unique<std::atomic<uint32_t>[]>(kPages);
        for (int i = 0; i < 3; ++i) g_viewBase[i] = reinterpret_cast<uintptr_t>(Memory::Translate(kViews[i]));
#ifdef _WIN32
        AddVectoredExceptionHandler(1, Handler);
#else
        // macOS/iOS deliver protection faults on mapped pages as SIGBUS, Linux as SIGSEGV.
        struct sigaction sa{};
        sa.sa_sigaction = SignalHandler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &g_prevSegv);
        sigaction(SIGBUS, &sa, &g_prevBus);
#endif
        Memory::g_commitHook = OnCommit;
    }

    void Protect(uint32_t physical, uint32_t size)
    {
        if (!g_protected || !size) return;
        physical &= kPhysicalSize - 1;
        const uint32_t first = physical >> kPageShift;
        const uint32_t last = std::min<uint32_t>((uint64_t(physical) + size - 1) >> kPageShift, kPages - 1);
        std::lock_guard lock(g_protectMutex);
        // Only pages not yet protected need a protect call (runs of them, one call each).
        uint32_t page = first;
        while (page <= last)
        {
            if (g_protected[page].load()) { ++page; continue; }
            uint32_t end = page;
            while (end + 1 <= last && !g_protected[end + 1].load()) ++end;
            for (uint32_t p = page; p <= end; ++p) g_protected[p] = 1;
            SetPages(page, end - page + 1, Prot::ReadOnly);
            page = end + 1;
        }
    }

    uint32_t Epoch() { return g_epoch.load(); }
    uint64_t Faults() { return g_faults.load(); }

    bool WrittenSince(uint32_t physical, uint32_t size, uint32_t epoch)
    {
        if (!g_protected || !size) return false;
        physical &= kPhysicalSize - 1;
        const uint32_t first = physical >> kPageShift;
        const uint32_t last = std::min<uint32_t>((uint64_t(physical) + size - 1) >> kPageShift, kPages - 1);
        for (uint32_t p = first; p <= last; ++p)
            if (g_lastWrite[p].load(std::memory_order_relaxed) > epoch) return true;
        return false;
    }

    void HostWillWrite(uint32_t guestAddr, uint32_t size)
    {
        if (g_vSize && size && guestAddr < g_vBase + g_vSize && guestAddr + size > g_vBase) // debug virtual watch: release the pages
        {
            std::lock_guard lock(g_vMutex);
            const uint32_t a = std::max(guestAddr, g_vBase), b = std::min(guestAddr + size, g_vBase + g_vSize);
            for (uint32_t p = (a - g_vBase) >> kPageShift; p <= (b - 1 - g_vBase) >> kPageShift; ++p)
                if (g_vArmed[p]) { g_vArmed[p] = 0; HostProtect(Memory::Translate(g_vBase + (p << kPageShift)), kPageSize, Prot::ReadWrite); }
        }
        if (!g_protected || !size || guestAddr < kViews[0]) return; // only the physical views are tracked
        const uint32_t physical = guestAddr & (kPhysicalSize - 1);
        const uint32_t first = physical >> kPageShift;
        const uint32_t last = std::min<uint32_t>((uint64_t(physical) + size - 1) >> kPageShift, kPages - 1);
        for (uint32_t p = first; p <= last; ++p) RecordWrite(p);
    }
}

namespace MemoryWatch
{
    void ReportNextWrite(uint32_t physical, uint32_t size)
    {
        if (!g_protected || !size) return;
        physical &= kPhysicalSize - 1;
        const uint32_t first = physical >> kPageShift;
        const uint32_t count = std::min<uint32_t>((size + kPageSize - 1) >> kPageShift, kPages - first);
        std::lock_guard lock(g_protectMutex);
        if (!g_report) g_report = std::make_unique<std::atomic<uint8_t>[]>(kPages);
        g_reportFirst = first;
        g_reportCount = count;
        g_reportsLeft = 80;
        for (uint32_t p = first; p < first + count; ++p) g_report[p] = 1;
        SetPages(first, count, Prot::ReadOnly);
        std::printf("[memwatch] reporting the next write to physical 0x%08X..0x%08X\n", physical, physical + count * kPageSize);
    }
}

namespace MemoryWatch
{
    void ReportVirtualWrites(uint32_t guestAddr, uint32_t size)
    {
        std::lock_guard lock(g_vMutex);
        g_vBase = guestAddr & ~(kPageSize - 1);
        g_vSize = (size + kPageSize - 1) & ~(kPageSize - 1);
        g_vArmed.assign(g_vSize >> kPageShift, 0);
        g_vReportsLeft = 120;
        uint32_t armed = 0;
        for (uint32_t p = 0; p < (g_vSize >> kPageShift); ++p)
            if (HostProtect(Memory::Translate(g_vBase + (p << kPageShift)), kPageSize, Prot::ReadOnly)) { g_vArmed[p] = 1; ++armed; }
        std::printf("[memwatch] reporting writers of virtual 0x%08X..0x%08X (%u committed pages armed)\n", g_vBase, g_vBase + g_vSize, armed);
    }
}

namespace MemoryWatch
{
    void ReportAccess(uint32_t physical, uint32_t size)
    {
        if (!g_protected || !size || g_accessSize) return;
        physical &= kPhysicalSize - 1;
        g_accessBase = physical & ~(kPageSize - 1);
        g_accessSize = (size + kPageSize - 1) & ~(kPageSize - 1);
        for (uintptr_t v : g_viewBase)
            HostProtect(reinterpret_cast<void*>(v + g_accessBase), g_accessSize, Prot::NoAccess);
        std::printf("[memwatch] reporting the first access to physical 0x%08X..0x%08X\n", g_accessBase, g_accessBase + g_accessSize);
    }
}

namespace MemoryWatch
{
    void UntrackedWrite(uint32_t physical, uint32_t size, void (*write)(uint8_t* p, uint32_t size, void* user), void* user)
    {
        physical &= kPhysicalSize - 1;
        const uint32_t first = physical >> kPageShift;
        const uint32_t last = std::min<uint32_t>((uint64_t(physical) + size - 1) >> kPageShift, kPages - 1);
        std::lock_guard lock(g_protectMutex);
        // Open view A for the write (the other views stay protected), then restore.
        void* page0 = reinterpret_cast<void*>(g_viewBase[0] + (uintptr_t(first) << kPageShift));
        const size_t bytes = size_t(last - first + 1) << kPageShift;
        HostProtect(page0, bytes, Prot::ReadWrite);
        write(reinterpret_cast<uint8_t*>(g_viewBase[0] + physical), size, user);
        for (uint32_t p = first; p <= last; ++p)
            if (g_protected && g_protected[p].load()) SetPages(p, 1, Prot::ReadOnly);
    }
}
