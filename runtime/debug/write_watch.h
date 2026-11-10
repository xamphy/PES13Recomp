#pragma once

namespace WriteWatch
{
    // Debug (--watch-wptr): report every store that moves CP_RB_WPTR backwards.
    void EnableWptrWatch();
    void PrintRecent();
}

namespace WriteWatch
{
    // Debug: reports the first store that writes '%' at guestAddr (page protection).
    void WatchFirstPercent(unsigned int guestAddr);
}

namespace WriteWatch
{
    // Debug (PES_WATCH_PHYS): report the first non-zero store to a guest physical page.
    void WatchPhysicalPage(unsigned int physAddr);
}

namespace WriteWatch
{
    void WatchVirtualPage(unsigned int guestAddr);
}

namespace WriteWatch
{
    void WatchFirstRead(unsigned int guestAddr);
}

namespace WriteWatch
{
    void WatchStores(unsigned int guestAddr, int reports);
}
