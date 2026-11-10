#pragma once
// Vertex input location for a D3D vertex declaration element (usage, usage index).
// Shared by XenosRecomp (shader generation) and the runtime (pipeline vertex input),
// so shaders and pipelines always agree. Uses locations 0-30: Metal (MoltenVK)
// allows at most 31 vertex attributes.
//
// D3DDECLUSAGE: POSITION 0, BLENDWEIGHT 1, BLENDINDICES 2, NORMAL 3, PSIZE 4, TEXCOORD 5,
//               TANGENT 6, BINORMAL 7, TESSFACTOR 8, POSITIONT 9, COLOR 10, FOG 11, DEPTH 12, SAMPLE 13
#include <cstdint>

constexpr int32_t PesVertexLocation(uint32_t usage, uint32_t usageIndex)
{
    switch (usage)
    {
    case 0:  return usageIndex < 2 ? int32_t(0 + usageIndex) : -1;   // POSITION0-1    -> 0-1
    case 1:  return usageIndex < 2 ? int32_t(2 + usageIndex) : -1;   // BLENDWEIGHT0-1 -> 2-3
    case 2:  return usageIndex < 2 ? int32_t(4 + usageIndex) : -1;   // BLENDINDICES0-1-> 4-5
    case 3:  return usageIndex < 2 ? int32_t(6 + usageIndex) : -1;   // NORMAL0-1      -> 6-7
    case 6:  return usageIndex == 0 ? 8 : -1;                        // TANGENT0       -> 8
    case 7:  return usageIndex == 0 ? 9 : -1;                        // BINORMAL0      -> 9
    case 10: return usageIndex < 4 ? int32_t(10 + usageIndex) : -1;  // COLOR0-3       -> 10-13
    case 5:  return usageIndex < 14 ? int32_t(14 + usageIndex) : -1; // TEXCOORD0-13   -> 14-27
    case 4:  return usageIndex == 0 ? 28 : -1;                       // PSIZE0         -> 28
    case 11: return usageIndex == 0 ? 29 : -1;                       // FOG0           -> 29
    case 9:  return usageIndex == 0 ? 30 : -1;                       // POSITIONT0     -> 30
    default: return -1;
    }
}
