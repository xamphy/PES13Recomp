// Non-Windows builds: the page-guard debug watches (write_watch.cpp) are Windows-only debug
// tools; these no-ops keep their call sites compiling. Real write tracking for the renderer
// lives in gpu/memory_watch.cpp.
#include "write_watch.h"

#include <cstdio>

namespace WriteWatch
{
    static void Unsupported(const char* what)
    {
        static bool warned = false;
        if (!warned) { warned = true; std::fprintf(stderr, "[watch] %s: debug watches are Windows-only\n", what); }
    }

    void EnableWptrWatch() { Unsupported("EnableWptrWatch"); }
    void PrintRecent() {}
    void WatchFirstPercent(unsigned int) { Unsupported("WatchFirstPercent"); }
    void WatchPhysicalPage(unsigned int) { Unsupported("WatchPhysicalPage"); }
    void WatchVirtualPage(unsigned int) { Unsupported("WatchVirtualPage"); }
    void WatchFirstRead(unsigned int) { Unsupported("WatchFirstRead"); }
    void WatchStores(unsigned int, int) { Unsupported("WatchStores"); }
}
