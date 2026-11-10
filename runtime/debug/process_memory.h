#pragma once
#include <cstdint>

// Memory use of this process, for the [mem] report.
namespace ProcessMemory
{
    struct Info
    {
        // Windows: private commit (PrivateUsage). Apple: phys_footprint - the number iOS/iPadOS
        // compares against the per-app limit before killing the app (includes GPU memory there).
        uint64_t footprint = 0;
        uint64_t footprintPeak = 0;  // Windows: PeakPagefileUsage; Apple: ledger peak (lifetime max)
        uint64_t resident = 0;       // working set / resident size
        uint64_t residentPeak = 0;
    };
    Info Get();
}
