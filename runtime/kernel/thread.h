#pragma once
#include <ppc_context.h>

#include "objects.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace Kernel
{
    // Guest-visible state of one guest thread. Layouts follow Xenia's X_KPCR /
    // X_KTHREAD; only the fields titles are known to read are filled in.
    struct GuestThread
    {
        ObjectPtr object;          // waitable thread object (guestObject = kthread)

        // Start/suspend handling (ExCreateThread with CREATE_SUSPENDED, NtResumeThread).
        std::mutex suspendMutex;
        std::condition_variable suspendCv;
        uint32_t suspendCount = 0;
        uint32_t exitCode = 0;
        uint32_t affinity = 0x3F; // KeSetAffinityThread; PCR+0x10C = lowest set bit
        int32_t heldCpuLock = -1;  // CPU whose interrupt lock this thread holds (MSR[EE] = 0)

        // Diagnostics (tools: watchdog report). Written by the thread itself.
        char name[32] = {};
        std::atomic<const char*> waitKind{ nullptr }; // nullptr = running guest code
        std::atomic<uint32_t> waitObject{ 0 };
        std::atomic<uint32_t> waitCaller{ 0 };        // guest return address of the blocking import
        std::atomic<bool> exited{ false };
        uint32_t hostThreadId = 0; // OS thread id while running (for sampling)
        PPCContext* context = nullptr; // live guest registers (for stack walks)

        uint32_t threadId = 0;
        uint32_t pcr = 0;          // r13 points here
        uint32_t kthread = 0;      // X_KTHREAD, PCR+0x100 points here
        uint32_t stackBase = 0;    // highest address (stack grows down from here)
        uint32_t stackLimit = 0;   // lowest usable address
        uint32_t tlsStatic = 0;    // PCR+0x00: __declspec(thread) data, copied from the XEX template
        uint32_t tlsSlots = 0;     // KeTlsAlloc'd slots (one dword each), right after the static data
        uint32_t tlsSlotCount = 0;
    };

    // Allocates stack, PCR, KTHREAD and TLS for a new guest thread.
    GuestThread* CreateGuestThread(uint32_t stackSize);

    // Sets up r1 (stack) and r13 (PCR) for running guest code on `thread`.
    void InitGuestContext(const GuestThread& thread, PPCContext& ctx);

    // The guest thread running on the calling host thread (nullptr if none).
    GuestThread* CurrentGuestThread();
    void SetCurrentGuestThread(GuestThread* thread);

    // Guest address of TLS slot `index` for `thread` (0 if out of range).
    uint32_t TlsSlotAddress(const GuestThread& thread, uint32_t index);

    // Thrown by ExTerminateThread to unwind the host stack back to the thread entry.
    struct GuestThreadExit
    {
        uint32_t exitCode;
    };

    // Runs guest code at `entry` on the calling host thread as `thread`
    // (r3/r4 = arguments). Handles GuestThreadExit. Returns the exit code.
    uint32_t RunGuestThread(GuestThread& thread, uint32_t entry, uint32_t arg0, uint32_t arg1);

    // Calls guest function `func` (r3/r4 = arguments) on the calling host thread,
    // which must already be bound to `thread` (e.g. a runtime-owned service thread
    // such as the graphics interrupt dispatcher). Arguments go in r3-r6. Returns r3.
    uint64_t CallGuestFunction(GuestThread& thread, uint32_t func, uint64_t arg0 = 0, uint64_t arg1 = 0,
        uint64_t arg2 = 0, uint64_t arg3 = 0, bool interruptsDisabled = false);

    // Marks the current guest thread as blocked for the scope's lifetime (watchdog report).
    struct WaitScope
    {
        WaitScope(const char* kind, uint32_t object, uint64_t callerLr);
        ~WaitScope();
        GuestThread* thread;
    };

    // Prints every guest thread and what it is blocked on.
    void DumpGuestThreads();
}
