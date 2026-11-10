#include "objects.h"
#include "thread.h"

#include <guest_memory.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>

namespace
{
    using namespace Kernel;

    std::mutex g_dispatcherMutex;

    std::mutex g_handleMutex;
    std::unordered_map<uint32_t, ObjectPtr> g_handles;
    uint32_t g_nextHandle = 0xF8000004;

    std::mutex g_guestMapMutex;
    std::unordered_map<uint32_t, ObjectPtr> g_guestObjects;

    // DISPATCHER_HEADER: u8 Type, u8 Absolute, u8 Size, u8 Inserted, s32 SignalState, LIST_ENTRY WaitList
    constexpr uint32_t HDR_TYPE = 0x0, HDR_SIZE = 0x2, HDR_SIGNAL = 0x4, HDR_WAITLIST = 0x8;

    void WriteGuestSignalState(const KObject& o)
    {
        if (o.guestObject)
            Memory::Store32(o.guestObject + HDR_SIGNAL, uint32_t(o.signalState));
    }

    uint32_t AllocHeader(uint8_t type, uint32_t size)
    {
        const uint32_t h = Memory::AllocKernel(size, 16);
        Memory::Store8(h + HDR_TYPE, type);
        Memory::Store8(h + HDR_SIZE, uint8_t(size / 4));
        Memory::Store32(h + HDR_WAITLIST + 0, h + HDR_WAITLIST); // empty wait list
        Memory::Store32(h + HDR_WAITLIST + 4, h + HDR_WAITLIST);
        return h;
    }

    // --- signal checks (dispatcher mutex held) ---
    bool IsSignaled(const KObject& o, GuestThread* self)
    {
        switch (o.kind)
        {
        case ObjectKind::Mutant: return o.owner == nullptr || o.owner == self;
        default:                 return o.signalState > 0;
        }
    }

    void Acquire(KObject& o, GuestThread* self)
    {
        switch (o.kind)
        {
        case ObjectKind::Event:
        case ObjectKind::Timer:
            if (!o.manualReset) o.signalState = 0;
            break;
        case ObjectKind::Semaphore:
            --o.signalState;
            break;
        case ObjectKind::Mutant:
            o.owner = self;
            ++o.recursion;
            o.signalState = 0;
            break;
        default:
            break;
        }
        WriteGuestSignalState(o);
    }

    uint64_t NowFileTime()
    {
        using namespace std::chrono;
        constexpr uint64_t UnixEpochAsFileTime = 116444736000000000ull;
        return UnixEpochAsFileTime + duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100;
    }
}

namespace Kernel
{
    // ---------------------------------------------------------------- handles
    uint32_t CreateHandle(const ObjectPtr& object)
    {
        std::lock_guard lock(g_handleMutex);
        const uint32_t handle = g_nextHandle;
        g_nextHandle += 4;
        g_handles[handle] = object;
        return handle;
    }

    ObjectPtr LookupHandle(uint32_t handle)
    {
        if (handle == CURRENT_THREAD_HANDLE)
        {
            GuestThread* t = CurrentGuestThread();
            return t && t->object ? t->object : nullptr;
        }
        std::lock_guard lock(g_handleMutex);
        const auto it = g_handles.find(handle);
        return it != g_handles.end() ? it->second : nullptr;
    }

    bool CloseHandle(uint32_t handle)
    {
        std::lock_guard lock(g_handleMutex);
        return g_handles.erase(handle) != 0;
    }

    // ---------------------------------------------------------------- objects
    void RegisterGuestObject(const ObjectPtr& object)
    {
        if (!object->guestObject)
            return;
        std::lock_guard lock(g_guestMapMutex);
        g_guestObjects[object->guestObject] = object;
    }

    ObjectPtr CreateEvent(bool manualReset, bool initialState)
    {
        auto o = std::make_shared<KObject>();
        o->kind = ObjectKind::Event;
        o->manualReset = manualReset;
        o->signalState = initialState ? 1 : 0;
        o->guestObject = AllocHeader(manualReset ? DISPATCHER_NOTIFICATION_EVENT : DISPATCHER_SYNCHRONIZATION_EVENT, 0x10);
        WriteGuestSignalState(*o);
        RegisterGuestObject(o);
        return o;
    }

    ObjectPtr CreateSemaphore(int32_t initialCount, int32_t maximumCount)
    {
        auto o = std::make_shared<KObject>();
        o->kind = ObjectKind::Semaphore;
        o->signalState = initialCount;
        o->limit = maximumCount;
        o->guestObject = AllocHeader(DISPATCHER_SEMAPHORE, 0x14);
        Memory::Store32(o->guestObject + 0x10, uint32_t(maximumCount));
        WriteGuestSignalState(*o);
        RegisterGuestObject(o);
        return o;
    }

    ObjectPtr CreateMutant(bool initialOwner)
    {
        auto o = std::make_shared<KObject>();
        o->kind = ObjectKind::Mutant;
        if (initialOwner)
        {
            o->owner = CurrentGuestThread();
            o->recursion = 1;
        }
        o->signalState = initialOwner ? 0 : 1;
        o->guestObject = AllocHeader(DISPATCHER_MUTANT, 0x20);
        WriteGuestSignalState(*o);
        RegisterGuestObject(o);
        return o;
    }

    ObjectPtr ObjectFromGuest(uint32_t guestObject)
    {
        if (!guestObject)
            return nullptr;

        std::lock_guard lock(g_guestMapMutex);
        if (const auto it = g_guestObjects.find(guestObject); it != g_guestObjects.end())
            return it->second;

        // A dispatcher object the title initialized itself (KeInitializeEvent is
        // usually inlined): adopt it, taking type and state from its header.
        auto o = std::make_shared<KObject>();
        o->guestObject = guestObject;
        const uint8_t type = Memory::Load8(guestObject + HDR_TYPE);
        o->signalState = int32_t(Memory::Load32(guestObject + HDR_SIGNAL));
        switch (type)
        {
        case DISPATCHER_NOTIFICATION_EVENT:    o->kind = ObjectKind::Event; o->manualReset = true; break;
        case DISPATCHER_SYNCHRONIZATION_EVENT: o->kind = ObjectKind::Event; o->manualReset = false; break;
        case DISPATCHER_SEMAPHORE:             o->kind = ObjectKind::Semaphore; o->limit = int32_t(Memory::Load32(guestObject + 0x10)); break;
        case DISPATCHER_MUTANT:                o->kind = ObjectKind::Mutant; o->signalState = 1; break;
        case DISPATCHER_NOTIFICATION_TIMER:    o->kind = ObjectKind::Timer; o->manualReset = true; break;
        case DISPATCHER_SYNCHRONIZATION_TIMER: o->kind = ObjectKind::Timer; o->manualReset = false; break;
        default:
            std::fprintf(stderr, "[kernel] adopting guest object 0x%08X with unknown dispatcher type %u\n", guestObject, type);
            o->kind = ObjectKind::Event;
            break;
        }
        g_guestObjects[guestObject] = o;
        return o;
    }

    // ------------------------------------------------------------- dispatcher
    // Every wait registers a Waiter on each object it waits for; a signal-state change wakes only
    // that object's waiters. (One global condition variable woke every blocked guest thread - ~30
    // of them - on every signal: ~0.09 cores per idle thread, ~1.8 cores in a match.)
    struct Waiter
    {
        std::condition_variable cv;
        bool woken = false;
    };

    std::mutex& DispatcherMutex() { return g_dispatcherMutex; }

    // A/B switch: PES_SYNC_BROADCAST=1 / control "sync broadcast" = old behaviour (wake every waiter).
    std::atomic<bool> g_broadcast{ std::getenv("PES_SYNC_BROADCAST") != nullptr };
    std::vector<Waiter*> g_allWaiters; // guarded by g_dispatcherMutex

    void SetSyncBroadcast(bool on)
    {
        g_broadcast = on;
        std::printf("[sync] wakeups: %s\n", on ? "broadcast" : "targeted");
    }

    void NotifyObject(KObject& o)
    {
        for (Waiter* w : g_broadcast.load(std::memory_order_relaxed) ? g_allWaiters : o.waiters)
        {
            w->woken = true;
            w->cv.notify_one();
        }
    }

    int32_t SetEvent(KObject& e)
    {
        std::lock_guard lock(g_dispatcherMutex);
        const int32_t previous = e.signalState;
        e.signalState = 1;
        WriteGuestSignalState(e);
        NotifyObject(e);
        return previous;
    }

    int32_t ResetEvent(KObject& e)
    {
        std::lock_guard lock(g_dispatcherMutex);
        const int32_t previous = e.signalState;
        e.signalState = 0;
        WriteGuestSignalState(e);
        return previous;
    }

    int32_t PulseEvent(KObject& e)
    {
        // Approximation: wake current waiters by signalling briefly. Waiters re-check
        // under the dispatcher lock, so give them a moment before resetting.
        const int32_t previous = SetEvent(e);
        std::this_thread::yield();
        ResetEvent(e);
        return previous;
    }

    uint32_t ReleaseSemaphore(KObject& s, int32_t count, int32_t* previous)
    {
        std::lock_guard lock(g_dispatcherMutex);
        if (previous) *previous = s.signalState;
        if (count <= 0 || s.signalState + count > s.limit)
            return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        s.signalState += count;
        WriteGuestSignalState(s);
        NotifyObject(s);
        return STATUS_SUCCESS;
    }

    uint32_t ReleaseMutant(KObject& m, int32_t* previous)
    {
        std::lock_guard lock(g_dispatcherMutex);
        if (previous) *previous = m.owner ? -int32_t(m.recursion) + 1 : 1;
        if (m.owner != CurrentGuestThread())
            return STATUS_MUTANT_NOT_OWNED;
        if (--m.recursion == 0)
        {
            m.owner = nullptr;
            m.signalState = 1;
            WriteGuestSignalState(m);
            NotifyObject(m);
        }
        return STATUS_SUCCESS;
    }

    void SignalThreadExit(KObject& t)
    {
        std::lock_guard lock(g_dispatcherMutex);
        t.signalState = 1;
        WriteGuestSignalState(t);
        NotifyObject(t);
    }

    bool ReadGuestTimeout(uint32_t timeoutPtr, int64_t& out)
    {
        if (!timeoutPtr)
            return false;
        out = int64_t(Memory::Load64(timeoutPtr));
        return true;
    }

    uint32_t WaitForObjects(const std::vector<ObjectPtr>& objects, bool waitAll, const int64_t* timeout)
    {
        for (const auto& o : objects)
            if (!o) return STATUS_INVALID_HANDLE;

        GuestThread* self = CurrentGuestThread();
        using clock = std::chrono::steady_clock;
        bool infinite = timeout == nullptr;
        clock::time_point deadline{};
        if (!infinite)
        {
            int64_t relative100ns = *timeout < 0 ? -*timeout
                : *timeout == 0 ? 0 : std::max<int64_t>(0, *timeout - int64_t(NowFileTime()));
            deadline = clock::now() + std::chrono::nanoseconds(relative100ns * 100);
        }

        std::unique_lock lock(g_dispatcherMutex);
        Waiter waiter;
        bool registered = false;
        auto finish = [&](uint32_t status)
        {
            if (registered)
            {
                for (const auto& o : objects)
                {
                    auto it = std::find(o->waiters.begin(), o->waiters.end(), &waiter);
                    if (it != o->waiters.end()) o->waiters.erase(it);
                }
                g_allWaiters.erase(std::find(g_allWaiters.begin(), g_allWaiters.end(), &waiter));
            }
            return status;
        };
        for (;;)
        {
            if (waitAll)
            {
                bool all = true;
                for (const auto& o : objects)
                    all = all && IsSignaled(*o, self);
                if (all)
                {
                    for (const auto& o : objects)
                        Acquire(*o, self);
                    return finish(STATUS_SUCCESS);
                }
            }
            else
            {
                for (size_t i = 0; i < objects.size(); ++i)
                {
                    if (IsSignaled(*objects[i], self))
                    {
                        Acquire(*objects[i], self);
                        return finish(STATUS_SUCCESS + uint32_t(i));
                    }
                }
            }

            if (!infinite && clock::now() >= deadline)
                return finish(STATUS_TIMEOUT);
            if (!registered)
            {
                for (const auto& o : objects)
                    o->waiters.push_back(&waiter);
                g_allWaiters.push_back(&waiter);
                registered = true;
            }
            waiter.woken = false;
            if (infinite)
                waiter.cv.wait(lock, [&] { return waiter.woken; });
            else
                waiter.cv.wait_until(lock, deadline, [&] { return waiter.woken; });
        }
    }
}
