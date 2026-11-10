// Events, semaphores, mutants, waits, critical sections and spinlocks.
#include "kernel.h"
#include "objects.h"
#include "thread.h"

#include <guest_memory.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace Kernel;

namespace
{
    void StoreHandle(uint32_t handlePtr, const ObjectPtr& o)
    {
        if (handlePtr) Memory::Store32(handlePtr, CreateHandle(o));
    }

    ObjectPtr HandleOfKind(uint32_t handle, ObjectKind kind)
    {
        ObjectPtr o = LookupHandle(handle);
        return o && o->kind == kind ? o : nullptr;
    }

    // Signal half of NtSignalAndWaitForSingleObjectEx.
    uint32_t SignalObject(KObject& o)
    {
        switch (o.kind)
        {
        case ObjectKind::Event:     SetEvent(o); return STATUS_SUCCESS;
        case ObjectKind::Semaphore: return ReleaseSemaphore(o, 1, nullptr);
        case ObjectKind::Mutant:    return ReleaseMutant(o, nullptr);
        default:                    return STATUS_OBJECT_TYPE_MISMATCH;
        }
    }

    uint32_t WaitOne(const ObjectPtr& o, uint32_t timeoutPtr)
    {
        int64_t timeout;
        const bool hasTimeout = ReadGuestTimeout(timeoutPtr, timeout);
        return WaitForObjects({ o }, false, hasTimeout ? &timeout : nullptr);
    }
}

// ------------------------------------------------------------------ events
// NTSTATUS NtCreateEvent(u32* Handle, OBJECT_ATTRIBUTES*, u32 EventType, u32 InitialState)
// EventType 0 = NotificationEvent (manual reset), 1 = SynchronizationEvent (auto reset).
GUEST_IMPORT(NtCreateEvent)
{
    StoreHandle(ctx.r3.u32, CreateEvent(ctx.r5.u32 == 0, ctx.r6.u32 != 0));
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtSetEvent)
{
    ObjectPtr e = HandleOfKind(ctx.r3.u32, ObjectKind::Event);
    if (!e) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    const int32_t previous = SetEvent(*e);
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtClearEvent)
{
    ObjectPtr e = HandleOfKind(ctx.r3.u32, ObjectKind::Event);
    if (!e) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    ResetEvent(*e);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// s32 KeSetEvent(KEVENT*, s32 Increment, u8 Wait) -> previous state
GUEST_IMPORT(KeSetEvent)
{
    ObjectPtr e = ObjectFromGuest(ctx.r3.u32);
    ctx.r3.u64 = e ? uint32_t(SetEvent(*e)) : 0;
}

// s32 KeResetEvent(KEVENT*) -> previous state
GUEST_IMPORT(KeResetEvent)
{
    ObjectPtr e = ObjectFromGuest(ctx.r3.u32);
    ctx.r3.u64 = e ? uint32_t(ResetEvent(*e)) : 0;
}

// -------------------------------------------------------------- semaphores
// NTSTATUS NtCreateSemaphore(u32* Handle, OBJECT_ATTRIBUTES*, s32 InitialCount, s32 MaximumCount)
GUEST_IMPORT(NtCreateSemaphore)
{
    StoreHandle(ctx.r3.u32, CreateSemaphore(ctx.r5.s32, ctx.r6.s32));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtReleaseSemaphore(u32 Handle, s32 ReleaseCount, s32* PreviousCount)
GUEST_IMPORT(NtReleaseSemaphore)
{
    ObjectPtr s = HandleOfKind(ctx.r3.u32, ObjectKind::Semaphore);
    if (!s) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int32_t previous = 0;
    const uint32_t status = ReleaseSemaphore(*s, ctx.r4.s32, &previous);
    if (ctx.r5.u32) Memory::Store32(ctx.r5.u32, uint32_t(previous));
    ctx.r3.u64 = status;
}

// ------------------------------------------------------------------ mutants
// NTSTATUS NtCreateMutant(u32* Handle, OBJECT_ATTRIBUTES*, u32 InitialOwner)
GUEST_IMPORT(NtCreateMutant)
{
    StoreHandle(ctx.r3.u32, CreateMutant(ctx.r5.u32 != 0));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtReleaseMutant(u32 Handle, s32* PreviousCount)
GUEST_IMPORT(NtReleaseMutant)
{
    ObjectPtr m = HandleOfKind(ctx.r3.u32, ObjectKind::Mutant);
    if (!m) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    int32_t previous = 0;
    const uint32_t status = ReleaseMutant(*m, &previous);
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, uint32_t(previous));
    ctx.r3.u64 = status;
}

// -------------------------------------------------------------------- waits
// NTSTATUS NtWaitForSingleObjectEx(u32 Handle, u32 WaitMode, u32 Alertable, s64* Timeout)
GUEST_IMPORT(NtWaitForSingleObjectEx)
{
    WaitScope scope("NtWaitForSingleObjectEx handle", ctx.r3.u32, ctx.lr);
    ObjectPtr o = LookupHandle(ctx.r3.u32);
    ctx.r3.u64 = o ? WaitOne(o, ctx.r6.u32) : STATUS_INVALID_HANDLE;
}

// NTSTATUS KeWaitForSingleObject(void* Object, u32 WaitReason, u32 WaitMode, u32 Alertable, s64* Timeout)
GUEST_IMPORT(KeWaitForSingleObject)
{
    WaitScope scope("KeWaitForSingleObject object", ctx.r3.u32, ctx.lr);
    ObjectPtr o = ObjectFromGuest(ctx.r3.u32);
    ctx.r3.u64 = o ? WaitOne(o, ctx.r7.u32) : STATUS_INVALID_PARAMETER;
}

// NTSTATUS KeWaitForMultipleObjects(u32 Count, void** Objects, u32 WaitType, u32 WaitReason,
//                                   u32 WaitMode, u32 Alertable, s64* Timeout, KWAIT_BLOCK*)
// WaitType 0 = WaitAll, 1 = WaitAny.
GUEST_IMPORT(KeWaitForMultipleObjects)
{
    WaitScope scope("KeWaitForMultipleObjects array", ctx.r4.u32, ctx.lr);
    const uint32_t count = ctx.r3.u32, array = ctx.r4.u32;
    std::vector<ObjectPtr> objects;
    for (uint32_t i = 0; i < count; ++i)
        objects.push_back(ObjectFromGuest(Memory::Load32(array + i * 4)));
    int64_t timeout;
    const bool hasTimeout = ReadGuestTimeout(ctx.r9.u32, timeout);
    ctx.r3.u64 = WaitForObjects(objects, ctx.r5.u32 == 0, hasTimeout ? &timeout : nullptr);
}

// NTSTATUS NtSignalAndWaitForSingleObjectEx(u32 SignalHandle, u32 WaitHandle, u32 WaitMode,
//                                           u32 Alertable, s64* Timeout)
GUEST_IMPORT(NtSignalAndWaitForSingleObjectEx)
{
    WaitScope scope("NtSignalAndWait wait handle", ctx.r4.u32, ctx.lr);
    ObjectPtr signal = LookupHandle(ctx.r3.u32);
    ObjectPtr wait = LookupHandle(ctx.r4.u32);
    if (!signal || !wait) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    const uint32_t status = SignalObject(*signal);
    ctx.r3.u64 = status != STATUS_SUCCESS ? status : WaitOne(wait, ctx.r7.u32);
}

// --------------------------------------------------------- critical sections
// X_RTL_CRITICAL_SECTION: DISPATCHER_HEADER (0x10), s32 LockCount (-1 = free),
// s32 RecursionCount, KTHREAD* OwningThread. The guest fields are kept up to date
// (titles do look at OwningThread); the actual locking is host-side.
namespace
{
    struct HostCriticalSection
    {
        std::mutex mutex;
        std::condition_variable cv;
        GuestThread* owner = nullptr;
        int32_t recursion = 0;
    };

    std::mutex g_csMapMutex;
    std::unordered_map<uint32_t, std::unique_ptr<HostCriticalSection>> g_criticalSections;

    HostCriticalSection& GetCriticalSection(uint32_t cs)
    {
        std::lock_guard lock(g_csMapMutex);
        auto& entry = g_criticalSections[cs];
        if (!entry)
            entry = std::make_unique<HostCriticalSection>();
        return *entry;
    }

    void InitCriticalSection(uint32_t cs)
    {
        Memory::Store8(cs + 0x00, DISPATCHER_SYNCHRONIZATION_EVENT);
        Memory::Store32(cs + 0x04, 0);
        Memory::Store32(cs + 0x10, 0xFFFFFFFF); // LockCount
        Memory::Store32(cs + 0x14, 0);          // RecursionCount
        Memory::Store32(cs + 0x18, 0);          // OwningThread
        GetCriticalSection(cs);
    }

    void PublishCriticalSection(uint32_t cs, const HostCriticalSection& h)
    {
        Memory::Store32(cs + 0x10, uint32_t(h.recursion - 1));
        Memory::Store32(cs + 0x14, uint32_t(h.recursion));
        Memory::Store32(cs + 0x18, h.owner ? h.owner->kthread : 0);
    }
}

GUEST_IMPORT(RtlInitializeCriticalSection)            { InitCriticalSection(ctx.r3.u32); }
GUEST_IMPORT(RtlInitializeCriticalSectionAndSpinCount) { InitCriticalSection(ctx.r3.u32); ctx.r3.u64 = STATUS_SUCCESS; }

GUEST_IMPORT(RtlEnterCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    HostCriticalSection& h = GetCriticalSection(cs);
    GuestThread* self = CurrentGuestThread();
    WaitScope scope("RtlEnterCriticalSection", cs, ctx.lr);
    std::unique_lock lock(h.mutex);
    h.cv.wait(lock, [&] { return h.owner == nullptr || h.owner == self; });
    h.owner = self;
    ++h.recursion;
    PublishCriticalSection(cs, h);
}

// BOOL RtlTryEnterCriticalSection(RTL_CRITICAL_SECTION*)
GUEST_IMPORT(RtlTryEnterCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    HostCriticalSection& h = GetCriticalSection(cs);
    GuestThread* self = CurrentGuestThread();
    std::lock_guard lock(h.mutex);
    if (h.owner != nullptr && h.owner != self)
    {
        ctx.r3.u64 = 0;
        return;
    }
    h.owner = self;
    ++h.recursion;
    PublishCriticalSection(cs, h);
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(RtlLeaveCriticalSection)
{
    const uint32_t cs = ctx.r3.u32;
    HostCriticalSection& h = GetCriticalSection(cs);
    bool released = false;
    {
        std::lock_guard lock(h.mutex);
        if (h.owner != CurrentGuestThread() || h.recursion <= 0)
        {
            std::fprintf(stderr, "[sync] RtlLeaveCriticalSection(0x%08X) by a thread that does not own it\n", cs);
            return;
        }
        if (--h.recursion == 0)
        {
            h.owner = nullptr;
            released = true;
        }
        PublishCriticalSection(cs, h);
    }
    if (released)
        h.cv.notify_one();
}

// --------------------------------------------------------------- spinlocks
// Guest spinlocks are a dword: 0 = free, otherwise the owner's KTHREAD.
namespace
{
    void AcquireSpin(uint32_t lockAddr)
    {
        auto* word = Memory::Translate<uint32_t>(lockAddr);
        std::atomic_ref<uint32_t> ref(*word);
        GuestThread* self = CurrentGuestThread();
        const uint32_t value = __builtin_bswap32(self ? self->kthread : 1);
        for (uint32_t spins = 0;; ++spins)
        {
            uint32_t expected = 0;
            if (ref.compare_exchange_weak(expected, value, std::memory_order_acquire))
                return;
            if (spins > 64)
                std::this_thread::yield();
        }
    }

    void ReleaseSpin(uint32_t lockAddr)
    {
        std::atomic_ref<uint32_t> ref(*Memory::Translate<uint32_t>(lockAddr));
        ref.store(0, std::memory_order_release);
    }
}

// u32 KfAcquireSpinLock(u32* SpinLock) -> old IRQL
GUEST_IMPORT(KfAcquireSpinLock)                { AcquireSpin(ctx.r3.u32); ctx.r3.u64 = 0; }
// void KfReleaseSpinLock(u32* SpinLock, u32 OldIrql)
GUEST_IMPORT(KfReleaseSpinLock)                { ReleaseSpin(ctx.r3.u32); }
GUEST_IMPORT(KeAcquireSpinLockAtRaisedIrql)    { AcquireSpin(ctx.r3.u32); }
GUEST_IMPORT(KeReleaseSpinLockFromRaisedIrql)  { ReleaseSpin(ctx.r3.u32); }
