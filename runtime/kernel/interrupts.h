#pragma once
// Emulated interrupt masking per hardware thread.
//
// On the console an interrupt (or DPC) on CPU n preempts whatever runs on CPU n, and
// code protects its critical sections by clearing MSR[EE]. Here guest threads and the
// runtime's interrupt/DPC threads run in parallel, so each CPU has a lock instead:
//   - guest code holds CPU n's lock while MSR[EE] = 0 (see ppcOnMsrEEChange),
//   - interrupt handlers and DPCs targeting CPU n run while holding it.
#include <cstdint>
#include <mutex>

namespace Kernel
{
    constexpr uint32_t CpuCount = 6;

    std::recursive_mutex& CpuInterruptLock(uint32_t cpu);

    // Guest-visible processor number of the calling guest thread (PCR+0x10C).
    uint32_t CurrentCpu();
}
