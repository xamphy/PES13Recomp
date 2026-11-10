#pragma once
// Kernel objects, handles and the dispatcher (waits/signals).
//
// Every waitable object has a guest-visible DISPATCHER_HEADER in guest memory,
// because titles mix handle APIs (NtWaitForSingleObjectEx) with pointer APIs
// (ObReferenceObjectByHandle + KeWaitForSingleObject, or KEVENTs they allocate
// themselves). Host state is authoritative; the header's SignalState is kept in sync.
//
// Synchronization is deliberately simple: one dispatcher mutex + condition variable.
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace Kernel
{
    struct GuestThread;

    // DISPATCHER_HEADER.Type values
    enum : uint8_t
    {
        DISPATCHER_NOTIFICATION_EVENT    = 0,
        DISPATCHER_SYNCHRONIZATION_EVENT = 1,
        DISPATCHER_MUTANT                = 2,
        DISPATCHER_SEMAPHORE             = 5,
        DISPATCHER_THREAD                = 6,
        DISPATCHER_NOTIFICATION_TIMER    = 8,
        DISPATCHER_SYNCHRONIZATION_TIMER = 9,
    };

    enum : uint32_t
    {
        STATUS_SUCCESS                  = 0x00000000,
        STATUS_ABANDONED_WAIT_0         = 0x00000080,
        STATUS_USER_APC                 = 0x000000C0,
        STATUS_TIMEOUT                  = 0x00000102,
        STATUS_INVALID_HANDLE           = 0xC0000008,
        STATUS_INVALID_PARAMETER        = 0xC000000D,
        STATUS_OBJECT_TYPE_MISMATCH     = 0xC0000024,
        STATUS_MUTANT_NOT_OWNED         = 0xC0000046,
        STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047,
    };

    enum class ObjectKind { Event, Semaphore, Mutant, Thread, Timer, Other };

    struct Waiter; // objects.cpp: one blocked WaitForObjects call

    struct KObject
    {
        ObjectKind kind = ObjectKind::Other;
        uint32_t guestObject = 0;     // guest DISPATCHER_HEADER (or KTHREAD) address

        // Dispatcher state - guarded by DispatcherMutex().
        bool manualReset = true;      // events/timers
        int32_t signalState = 0;      // event: 0/1, semaphore: count, thread/timer: 0/1
        int32_t limit = 0;            // semaphore maximum
        GuestThread* owner = nullptr; // mutant
        uint32_t recursion = 0;       // mutant

        GuestThread* thread = nullptr; // ObjectKind::Thread

        // Threads currently blocked in WaitForObjects on this object (guarded by DispatcherMutex()):
        // a signal wakes only these, not every waiting thread.
        std::vector<Waiter*> waiters;

        virtual ~KObject() = default;
    };
    using ObjectPtr = std::shared_ptr<KObject>;

    // --- Handles ---------------------------------------------------------------
    uint32_t CreateHandle(const ObjectPtr& object);
    ObjectPtr LookupHandle(uint32_t handle);     // also resolves the current-thread pseudo handle
    bool CloseHandle(uint32_t handle);
    constexpr uint32_t CURRENT_THREAD_HANDLE = 0xFFFFFFFE;

    // --- Objects ---------------------------------------------------------------
    // Creates a dispatcher object with a fresh guest header of the given type.
    ObjectPtr CreateEvent(bool manualReset, bool initialState);
    ObjectPtr CreateSemaphore(int32_t initialCount, int32_t maximumCount);
    ObjectPtr CreateMutant(bool initialOwner);

    // Host object for a guest dispatcher object (creating it from the header on
    // first use, for KEVENTs etc. that the title allocated itself).
    ObjectPtr ObjectFromGuest(uint32_t guestObject);
    void RegisterGuestObject(const ObjectPtr& object);

    // --- Dispatcher ------------------------------------------------------------
    std::mutex& DispatcherMutex();
    void NotifyObject(KObject& o); // after a signal-state change of o; caller holds DispatcherMutex()
    void SetSyncBroadcast(bool on); // A/B: wake every waiter on every signal (old behaviour)

    // Event / semaphore / mutant operations (take the dispatcher lock internally).
    int32_t SetEvent(KObject& event);      // returns previous state
    int32_t ResetEvent(KObject& event);
    int32_t PulseEvent(KObject& event);
    uint32_t ReleaseSemaphore(KObject& semaphore, int32_t count, int32_t* previous);
    uint32_t ReleaseMutant(KObject& mutant, int32_t* previous);
    void SignalThreadExit(KObject& thread);

    // Timeout follows NT conventions: nullptr = infinite, negative = relative (100 ns
    // units), positive = absolute FILETIME, 0 = poll.
    // Returns STATUS_SUCCESS + index of the satisfying object (waitAny), or STATUS_TIMEOUT.
    uint32_t WaitForObjects(const std::vector<ObjectPtr>& objects, bool waitAll, const int64_t* timeout);

    // Reads an NT timeout from guest memory. Returns false if timeoutPtr is 0 (infinite).
    bool ReadGuestTimeout(uint32_t timeoutPtr, int64_t& out);
}
