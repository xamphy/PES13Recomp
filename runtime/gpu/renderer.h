#pragma once
// Vulkan renderer driven by the command processor (Xenia's design, lean): the CP feeds it
// register writes, shader loads, draws, resolves and swaps; it renders with shaders
// translated at runtime by Xenia's Xenos -> SPIR-V translator.
//
// Model (first version):
//   - render targets: one Vulkan image per (EDRAM base, format), 1x samples, no depth yet
//   - resolves copy a render-target rectangle into a "surface" image that stands for a
//     guest texture address range; textures at that address sample the surface
//   - other textures are uploaded from guest memory (untiled, endian-swapped)
//   - vertex data: guest memory is mirrored into one storage buffer per draw's ranges
//   - Swap presents the surface at the front-buffer address
// Everything runs on the command processor thread; one frame in flight.
#include <cstdint>

namespace Renderer
{
    // After Video::Init. Returns false without Vulkan (the CP then only counts draws).
    bool Init();
    bool Active();
    // Debug: after frame N, write render targets/surfaces/RGBA textures to userdata/*.ppm.
    void DumpSurfacesAtFrame(uint64_t frame);

    void WriteRegister(uint32_t index, uint32_t value);
    // stage 0 = vertex, 1 = pixel; dwords are logical microcode values.
    void SetShader(int stage, const uint32_t* dwords, uint32_t count, uint64_t hash);

    // A draw packet (DRAW_INDX / DRAW_INDX_2), after VGT_DRAW_INITIATOR was written.
    // For DMA indices: index base (physical address), raw size dword (count | endian << 30).
    void Draw(uint32_t indexBase, uint32_t indexSizeDword);

    // XE_SWAP: present the surface at the front buffer (physical) address. Returns false
    // if the renderer has nothing there (the CP then falls back to reading guest memory).
    // swizzle: the front buffer fetch constant's 12-bit swizzle (3 bits per component).
    bool Swap(uint32_t frontBufferPhysical, uint32_t width, uint32_t height, uint32_t swizzle);

    // Guest CPU code (XG untile) is about to read resolved memory at guestAddr: store it in the
    // byte order the CPU expects (see renderer.cpp, "CPU view of resolves"). Any thread.
    void PrepareResolveForCpu(uint32_t guestAddr);
}
