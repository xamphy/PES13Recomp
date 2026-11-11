// Thread creation/control and generic object-handle imports.
#include "kernel.h"
#include "objects.h"
#include "thread.h"
#include "host_qos.h"

#include <guest_memory.h>

#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
    constexpr uint32_t CREATE_SUSPENDED = 0x00000001;
    constexpr uint32_t DUPLICATE_CLOSE_SOURCE = 0x00000001;
}

// NTSTATUS ExCreateThread(u32* Handle, u32 StackSize, u32* ThreadId, u32 XapiThreadStartup,
//                         u32 StartAddress, u32 StartContext, u32 CreationFlags)
GUEST_IMPORT(ExCreateThread)
{
    const uint32_t handlePtr = ctx.r3.u32, stackSize = ctx.r4.u32, threadIdPtr = ctx.r5.u32;
    const uint32_t xapiStartup = ctx.r6.u32, startAddress = ctx.r7.u32, startContext = ctx.r8.u32;
    const uint32_t flags = ctx.r9.u32;

    Kernel::GuestThread* t = Kernel::CreateGuestThread(stackSize);
    if (!t)
    {
        ctx.r3.u64 = 0xC0000017; // STATUS_NO_MEMORY
        return;
    }
    t->suspendCount = (flags & CREATE_SUSPENDED) ? 1 : 0;

    const uint32_t handle = Kernel::CreateHandle(t->object);
    if (handlePtr) Memory::Store32(handlePtr, handle);
    if (threadIdPtr) Memory::Store32(threadIdPtr, t->threadId);

    std::printf("[thread] created thread %u (entry 0x%08X ctx 0x%08X%s)\n", t->threadId,
        startAddress, startContext, (flags & CREATE_SUSPENDED) ? ", suspended" : "");

    std::thread([t, xapiStartup, startAddress, startContext]
    {
        Kernel::SetHostThreadQos(Kernel::HostQos::Initiated); // Apple: prefer performance cores
        {
            std::unique_lock lock(t->suspendMutex);
            t->suspendCv.wait(lock, [t] { return t->suspendCount == 0; });
        }
        // With an XAPI startup routine the kernel calls Startup(StartAddress, StartContext);
        // it sets up the C runtime for the thread and then calls StartAddress(StartContext).
        if (xapiStartup)
            Kernel::RunGuestThread(*t, xapiStartup, startAddress, startContext);
        else
            Kernel::RunGuestThread(*t, startAddress, startContext, 0);
        std::printf("[thread] thread %u exited (code 0x%X)\n", t->threadId, t->exitCode);
    }).detach();

    ctx.r3.u64 = Kernel::STATUS_SUCCESS;
}

// void ExTerminateThread(u32 ExitStatus)
GUEST_IMPORT(ExTerminateThread)
{
    throw Kernel::GuestThreadExit{ ctx.r3.u32 };
}

// NTSTATUS NtResumeThread(u32 Handle, u32* SuspendCount)
GUEST_IMPORT(NtResumeThread)
{
    Kernel::ObjectPtr o = Kernel::LookupHandle(ctx.r3.u32);
    if (!o || o->kind != Kernel::ObjectKind::Thread)
    {
        ctx.r3.u64 = Kernel::STATUS_INVALID_HANDLE;
        return;
    }
    Kernel::GuestThread* t = o->thread;
    uint32_t previous;
    {
        std::lock_guard lock(t->suspendMutex);
        previous = t->suspendCount;
        if (t->suspendCount) --t->suspendCount;
    }
    t->suspendCv.notify_all();
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, previous);
    ctx.r3.u64 = Kernel::STATUS_SUCCESS;
}

// NTSTATUS NtSuspendThread(u32 Handle, u32* PreviousSuspendCount)
// Only self-suspension can be honoured (a host thread cannot be stopped at an
// arbitrary point safely); suspending another running thread is logged.
GUEST_IMPORT(NtSuspendThread)
{
    Kernel::ObjectPtr o = Kernel::LookupHandle(ctx.r3.u32);
    if (!o || o->kind != Kernel::ObjectKind::Thread)
    {
        ctx.r3.u64 = Kernel::STATUS_INVALID_HANDLE;
        return;
    }
    Kernel::GuestThread* t = o->thread;
    uint32_t previous;
    {
        std::lock_guard lock(t->suspendMutex);
        previous = t->suspendCount++;
    }
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, previous);

    if (t == Kernel::CurrentGuestThread())
    {
        std::unique_lock lock(t->suspendMutex);
        t->suspendCv.wait(lock, [t] { return t->suspendCount == 0; });
    }
    else
    {
        std::fprintf(stderr, "[thread] NtSuspendThread on running thread %u ignored\n", t->threadId);
        std::lock_guard lock(t->suspendMutex);
        t->suspendCount = previous;
    }
    ctx.r3.u64 = Kernel::STATUS_SUCCESS;
}

// u32 KeSetAffinityThread(KTHREAD* Thread, u32 Affinity, u32* PreviousAffinity)
// The host scheduler decides where threads really run, but the guest-visible processor
// number (PCR+0x10C) must follow the affinity: Direct3D and the CRT index per-processor
// data with it, and two threads that both claim CPU 0 corrupt each other's state.
GUEST_IMPORT(KeSetAffinityThread)
{
    Kernel::GuestThread* t = nullptr;
    if (Kernel::ObjectPtr o = Kernel::ObjectFromGuest(ctx.r3.u32); o && o->kind == Kernel::ObjectKind::Thread)
        t = o->thread;
    const uint32_t affinity = ctx.r4.u32 & 0x3F;

    if (ctx.r5.u32) Memory::Store32(ctx.r5.u32, t ? t->affinity : 0x3F);
    if (t && affinity)
    {
        if (t->affinity != affinity)
            std::printf("[thread] thread %u (%s) affinity 0x%02X (CPU %d) - set by thread %u\n", t->threadId, t->name, affinity,
                __builtin_ctz(affinity), Kernel::CurrentGuestThread() ? Kernel::CurrentGuestThread()->threadId : 0);
        t->affinity = affinity;
        Memory::Store8(t->pcr + 0x10C, uint8_t(__builtin_ctz(affinity)));
    }
    ctx.r3.u64 = Kernel::STATUS_SUCCESS;
}

// s32 KeSetBasePriorityThread(KTHREAD* Thread, s32 Increment) -> previous increment
GUEST_IMPORT(KeSetBasePriorityThread)
{
    ctx.r3.u64 = 0; // host scheduling is left to the OS
}

// s32 KeQueryBasePriorityThread(KTHREAD* Thread)
GUEST_IMPORT(KeQueryBasePriorityThread)
{
    ctx.r3.u64 = 0;
}

// NTSTATUS KeDelayExecutionThread(u32 WaitMode, u32 Alertable, s64* Interval)
GUEST_IMPORT(KeDelayExecutionThread)
{
    Kernel::WaitScope scope("KeDelayExecutionThread", 0, ctx.lr);
    int64_t interval = 0;
    Kernel::ReadGuestTimeout(ctx.r5.u32, interval);
    if (interval < 0)
        std::this_thread::sleep_for(std::chrono::nanoseconds(-interval * 100));
    else
        std::this_thread::yield();
    ctx.r3.u64 = Kernel::STATUS_SUCCESS;
}

// NTSTATUS ObReferenceObjectByHandle(u32 Handle, u32 ObjectType, u32* Object)
GUEST_IMPORT(ObReferenceObjectByHandle)
{
    Kernel::ObjectPtr o = Kernel::LookupHandle(ctx.r3.u32);
    if (ctx.r5.u32) Memory::Store32(ctx.r5.u32, o ? o->guestObject : 0);
    ctx.r3.u64 = o ? Kernel::STATUS_SUCCESS : Kernel::STATUS_INVALID_HANDLE;
}

// Reference counting is handled by shared_ptr on the host side.
GUEST_IMPORT(ObReferenceObject) {}
GUEST_IMPORT(ObDereferenceObject) {}

// NTSTATUS NtClose(u32 Handle)
GUEST_IMPORT(NtClose)
{
    const uint32_t handle = ctx.r3.u32;
    ctx.r3.u64 = (handle == Kernel::CURRENT_THREAD_HANDLE || Kernel::CloseHandle(handle))
        ? Kernel::STATUS_SUCCESS : Kernel::STATUS_INVALID_HANDLE;
}

// NTSTATUS NtDuplicateObject(u32 SourceHandle, u32* TargetHandle, u32 Options)
GUEST_IMPORT(NtDuplicateObject)
{
    Kernel::ObjectPtr o = Kernel::LookupHandle(ctx.r3.u32);
    if (!o)
    {
        ctx.r3.u64 = Kernel::STATUS_INVALID_HANDLE;
        return;
    }
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, Kernel::CreateHandle(o));
    if (ctx.r5.u32 & DUPLICATE_CLOSE_SOURCE) Kernel::CloseHandle(ctx.r3.u32);
    ctx.r3.u64 = Kernel::STATUS_SUCCESS;
}

// Critical regions only block kernel APCs, which this runtime does not deliver.
GUEST_IMPORT(KeEnterCriticalRegion) {}
GUEST_IMPORT(KeLeaveCriticalRegion) {}
