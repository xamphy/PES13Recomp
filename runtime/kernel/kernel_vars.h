#pragma once
#include <cstdint>

namespace Kernel
{
    // True if xboxkrnl.exe exports `ordinal` as a variable (per xboxkrnl_table.inc).
    bool IsKernelVariable(uint16_t ordinal);

    // Allocates and initializes the guest object for a kernel variable export and
    // returns its guest address (0 if the ordinal is not a variable). Idempotent.
    uint32_t ResolveKernelVariable(uint16_t ordinal);

    // Starts the host thread that keeps KeTimeStampBundle ticking.
    void StartTimeStampUpdater();
}
