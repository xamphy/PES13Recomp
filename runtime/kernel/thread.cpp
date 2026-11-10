#include "thread.h"
#include "xex_module.h"

#include <ppc_config.h>

#include <guest_memory.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef _WIN32
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#endif

namespace
{
    thread_local Kernel::GuestThread* t_current = nullptr;
    std::atomic<uint32_t> g_nextThreadId{ 1 };

    std::mutex g_registryMutex;
    std::vector<Kernel::GuestThread*> g_threads;

    // X_KPCR offsets
    constexpr uint32_t PCR_TLS_PTR       = 0x000;
    constexpr uint32_t PCR_SELF          = 0x030;
    constexpr uint32_t PCR_STACK_BASE    = 0x070;
    constexpr uint32_t PCR_STACK_LIMIT   = 0x074;
    constexpr uint32_t PCR_CURRENT_THREAD = 0x100;
    constexpr uint32_t PCR_CURRENT_CPU   = 0x10C;
    constexpr uint32_t PCR_SIZE          = 0x1000;

    // X_KTHREAD offsets
    constexpr uint32_t KTHREAD_TYPE        = 0x000; // dispatcher header: type 6 = thread
    constexpr uint32_t KTHREAD_STACK_BASE  = 0x05C;
    constexpr uint32_t KTHREAD_STACK_LIMIT = 0x060;
    constexpr uint32_t KTHREAD_TLS_ADDRESS = 0x068;
    constexpr uint32_t KTHREAD_THREAD_ID   = 0x14C;
    constexpr uint32_t KTHREAD_SIZE        = 0xB00;
}

namespace Kernel
{
    GuestThread* CreateGuestThread(uint32_t stackSize)
    {
        const ModuleInfo& module = ExecutableModule();
        auto* t = new GuestThread();
        t->threadId = g_nextThreadId++;

        // Stack
        stackSize = stackSize ? stackSize : module.stackSize;
        t->stackLimit = Memory::AllocStack(stackSize);
        t->stackBase = t->stackLimit + ((stackSize + 0xFFFF) & ~0xFFFFu);

        // TLS: [static __declspec(thread) data][dynamic KeTlsAlloc slots]
        const uint32_t staticSize = (module.tlsDataSize + 3) & ~3u;
        t->tlsSlotCount = module.tlsSlotCount;
        t->tlsStatic = Memory::AllocKernel(staticSize + t->tlsSlotCount * 4 + 16, 16);
        t->tlsSlots = t->tlsStatic + staticSize;
        if (module.tlsRawDataAddr && module.tlsRawDataSize)
            std::memcpy(Memory::Translate(t->tlsStatic), Memory::Translate(module.tlsRawDataAddr), module.tlsRawDataSize);

        // KTHREAD
        t->kthread = Memory::AllocKernel(KTHREAD_SIZE, 16);
        Memory::Store8(t->kthread + KTHREAD_TYPE, 6);
        Memory::Store32(t->kthread + KTHREAD_STACK_BASE, t->stackBase);
        Memory::Store32(t->kthread + KTHREAD_STACK_LIMIT, t->stackLimit);
        Memory::Store32(t->kthread + KTHREAD_TLS_ADDRESS, t->tlsStatic);
        Memory::Store32(t->kthread + KTHREAD_THREAD_ID, t->threadId);

        // PCR
        t->pcr = Memory::AllocKernel(PCR_SIZE, 0x1000);
        Memory::Store32(t->pcr + PCR_TLS_PTR, t->tlsStatic);
        Memory::Store32(t->pcr + PCR_SELF, t->pcr);
        Memory::Store32(t->pcr + PCR_STACK_BASE, t->stackBase);
        Memory::Store32(t->pcr + PCR_STACK_LIMIT, t->stackLimit);
        Memory::Store32(t->pcr + PCR_CURRENT_THREAD, t->kthread);
        Memory::Store8(t->pcr + PCR_CURRENT_CPU, 0);

        if (!t->stackLimit || !t->tlsStatic || !t->kthread || !t->pcr)
        {
            std::fprintf(stderr, "[thread] failed to allocate guest thread %u\n", t->threadId);
            delete t;
            return nullptr;
        }

        // Waitable thread object; its dispatcher header is the KTHREAD itself.
        t->object = std::make_shared<KObject>();
        t->object->kind = ObjectKind::Thread;
        t->object->guestObject = t->kthread;
        t->object->thread = t;
        RegisterGuestObject(t->object);

        std::snprintf(t->name, sizeof(t->name), "thread %u", t->threadId);
        {
            std::lock_guard lock(g_registryMutex);
            g_threads.push_back(t);
        }
        return t;
    }

    WaitScope::WaitScope(const char* kind, uint32_t object, uint64_t callerLr)
        : thread(CurrentGuestThread())
    {
        if (!thread) return;
        thread->waitObject = object;
        thread->waitCaller = uint32_t(callerLr);
        thread->waitKind = kind;
    }

    WaitScope::~WaitScope()
    {
        if (thread) thread->waitKind = nullptr;
    }

    void DumpGuestThreads()
    {
        std::lock_guard lock(g_registryMutex);
        std::printf("---- guest threads ----\n");
        for (GuestThread* t : g_threads)
        {
            if (t->exited)
                continue;
            const char* kind = t->waitKind.load();
            if (kind)
                std::printf("  %-3u %-24s [host %5u] %s 0x%08X (called from 0x%08X)\n", t->threadId, t->name, t->hostThreadId, kind,
                    t->waitObject.load(), t->waitCaller.load());
            else
            {
                std::printf("  %-3u %-24s [host %5u] running guest code", t->threadId, t->name, t->hostThreadId);
#ifdef _WIN32
                // Sample the host instruction pointer a few times; resolve with tools\addr2func.ps1.
                if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, t->hostThreadId))
                {
                    const auto module = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
                    unsigned long long samples[4] = {};
                    int count = 0;
                    for (int i = 0; i < 4; ++i)
                    {
                        // No printing while the target is suspended: it may hold the CRT output lock.
                        if (SuspendThread(h) == DWORD(-1)) break;
                        CONTEXT c{};
                        c.ContextFlags = CONTEXT_CONTROL;
                        if (GetThreadContext(h, &c))
                            samples[count++] = c.Rip - module;
                        ResumeThread(h);
                        Sleep(5);
                    }
                    ::CloseHandle(h);
                    std::printf(" - rva samples:");
                    for (int i = 0; i < count; ++i)
                        std::printf(" 0x%llX", samples[i]);
                }
#endif
                std::printf("\n");
            }
            // Guest call stack: return addresses saved at (caller frame - 8), frames linked through 0(r1).
            if (const PPCContext* c = t->context)
            {
                uint32_t frame = c->r1.u32;
                std::printf("        guest stack: lr 0x%08X", uint32_t(c->lr));
                for (int depth = 0; depth < 14; ++depth)
                {
                    if (frame < t->stackLimit || frame >= t->stackBase) break;
                    const uint32_t back = Memory::Load32(frame);
                    if (back <= frame || back >= t->stackBase) break;
                    std::printf(" <- 0x%08X", Memory::Load32(back - 8));
                    frame = back;
                }
                std::printf("\n");
            }
        }
        std::printf("-----------------------\n");
    }

    uint32_t RunGuestThread(GuestThread& thread, uint32_t entry, uint32_t arg0, uint32_t arg1)
    {
        SetCurrentGuestThread(&thread);
#ifdef _WIN32
        thread.hostThreadId = GetCurrentThreadId();
#endif

        PPCContext ctx{};
        InitGuestContext(thread, ctx);
        ctx.r3.u64 = arg0;
        ctx.r4.u64 = arg1;
        thread.context = &ctx;

        uint8_t* base = Memory::Base();
        PPCFunc* func = PPC_LOOKUP_FUNC(base, entry);
        try
        {
            func(ctx, base);
            thread.exitCode = ctx.r3.u32;
        }
        catch (const GuestThreadExit& exit)
        {
            thread.exitCode = exit.exitCode;
        }

        thread.context = nullptr;
        thread.exited = true;
        SignalThreadExit(*thread.object);
        return thread.exitCode;
    }

    uint64_t CallGuestFunction(GuestThread& thread, uint32_t func, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
        bool interruptsDisabled)
    {
        SetCurrentGuestThread(&thread);
#ifdef _WIN32
        thread.hostThreadId = GetCurrentThreadId();
#endif
        PPCContext ctx{};
        InitGuestContext(thread, ctx);
        ctx.r3.u64 = arg0;
        ctx.r4.u64 = arg1;
        ctx.r5.u64 = arg2;
        ctx.r6.u64 = arg3;
        if (interruptsDisabled)
            ctx.msr &= ~0x8000u; // interrupt context: MSR[EE] = 0 (the caller holds the CPU's interrupt lock)
        thread.context = &ctx;
        uint8_t* base = Memory::Base();
        PPCFunc* target = PPC_LOOKUP_FUNC(base, func);
        target(ctx, base);
        thread.context = nullptr;
        return ctx.r3.u64;
    }

    void InitGuestContext(const GuestThread& thread, PPCContext& ctx)
    {
        // Leave a small frame at the top: 0(r1) is the back chain (null = outermost frame).
        ctx.r1.u64 = thread.stackBase - 0x100;
        Memory::Store32(ctx.r1.u32, 0);
        ctx.r13.u64 = thread.pcr;
        ctx.fpscr.loadFromHost();
    }

    GuestThread* CurrentGuestThread() { return t_current; }
    void SetCurrentGuestThread(GuestThread* thread) { t_current = thread; }

    uint32_t TlsSlotAddress(const GuestThread& thread, uint32_t index)
    {
        return index < thread.tlsSlotCount ? thread.tlsSlots + index * 4 : 0;
    }
}
