#pragma once
// Direct3D wait loops (BlockOnFence, swap throttle, ring space, resource locks) spin through the
// helper sub_822EA8F8. d3d_wait.cpp makes long waits sleep instead of burning a core.
namespace D3DWait
{
    // true = spin like the console (PES_D3D_SPIN=1 at start; control "d3d spin|sleep").
    void SetSpin(bool spin);
}
