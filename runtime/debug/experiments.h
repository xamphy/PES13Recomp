#pragma once

namespace Experiments
{
    extern bool g_skipFenceWait; // --skip-fence-wait: Direct3D BlockOnFence returns immediately
    extern bool g_noVsyncWait;   // --no-vsync-wait: the CP skips Direct3D's GPU-side vsync wait
}
