#pragma once
#include <cstdint>

namespace KickTrace
{
    // Prints the most recent ring-position events (see kick_trace.cpp).
    void Dump();

    // Command processor: records that it read WPTR and will execute [rptr, wptr).
    void RecordCpRead(uint32_t rptr, uint32_t wptr);
}
