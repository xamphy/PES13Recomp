// Vd* (video driver) kernel imports. Behaviour follows what Xenia exposes to titles.
#include "gpu.h"

#include <kernel/kernel.h>
#include <guest_memory.h>

#include <cstdio>
#include <cstring>

namespace
{
    uint32_t FloatBits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }

    // Arguments past r10 live in the caller's parameter save area.
    uint32_t StackArg(const PPCContext& ctx, uint32_t index)
    {
        return Memory::Load32(ctx.r1.u32 + 0x54 + (index - 8) * 8);
    }

    void FillVideoMode(uint32_t m)
    {
        std::memset(Memory::Translate(m), 0, 0x30);
        Memory::Store32(m + 0x00, 1280);           // DisplayWidth
        Memory::Store32(m + 0x04, 720);            // DisplayHeight
        Memory::Store32(m + 0x08, 0);              // IsInterlaced
        Memory::Store32(m + 0x0C, 1);              // IsWidescreen
        Memory::Store32(m + 0x10, 1);              // IsHiDef
        Memory::Store32(m + 0x14, FloatBits(60));  // RefreshRate
        Memory::Store32(m + 0x18, 1);              // VideoStandard (NTSC)
        Memory::Store32(m + 0x1C, 0x4A);
        Memory::Store32(m + 0x20, 0x01);
    }

    constexpr uint32_t PacketType3(uint32_t opcode, uint32_t count)
    {
        return (3u << 30) | (((count - 1) & 0x3FFF) << 16) | ((opcode & 0x7F) << 8);
    }
}

// ------------------------------------------------------------ engine / ring buffer
GUEST_IMPORT(VdInitializeEngines)  { ctx.r3.u64 = 1; }
GUEST_IMPORT(VdShutdownEngines)    { ctx.r3.u64 = 0; }

// void VdSetGraphicsInterruptCallback(callback, user_data)
GUEST_IMPORT(VdSetGraphicsInterruptCallback)
{
    Gpu::SetInterruptCallback(ctx.r3.u32, ctx.r4.u32);
}

// void VdInitializeRingBuffer(u32 PhysicalAddress, u32 SizeLog2)
GUEST_IMPORT(VdInitializeRingBuffer)
{
    Gpu::InitializeRingBuffer(ctx.r3.u32, ctx.r4.u32);
}

// void VdEnableRingBufferRPtrWriteBack(u32 PhysicalAddress, u32 BlockSizeLog2)
GUEST_IMPORT(VdEnableRingBufferRPtrWriteBack)
{
    Gpu::EnableReadPointerWriteBack(ctx.r3.u32, ctx.r4.u32);
}

GUEST_IMPORT(VdSetSystemCommandBufferGpuIdentifierAddress) {}

// void VdGetSystemCommandBuffer(u32* Buffer, u32* Unknown)
GUEST_IMPORT(VdGetSystemCommandBuffer)
{
    if (ctx.r3.u32)
    {
        std::memset(Memory::Translate(ctx.r3.u32), 0, 0x94);
        Memory::Store32(ctx.r3.u32, 0xBEEF0000);
    }
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, 0xBEEF0001);
}

// u32 VdInitializeScalerCommandBuffer(... 9 args ..., void* Dest) - fills a buffer
// the title later submits; NOPs are enough since there is no hardware scaler.
GUEST_IMPORT(VdInitializeScalerCommandBuffer)
{
    const uint32_t dest = StackArg(ctx, 9);
    constexpr uint32_t dwords = 0x1CC / 4;
    if (dest)
        for (uint32_t i = 0; i < dwords; ++i)
            Memory::Store32(dest + i * 4, 0x80000000); // type-2 NOP
    ctx.r3.u64 = dwords;
}

GUEST_IMPORT(VdCallGraphicsNotificationRoutines) { ctx.r3.u64 = 0; }

// ------------------------------------------------------------------- display
// void VdQueryVideoMode(X_VIDEO_MODE*)
GUEST_IMPORT(VdQueryVideoMode) { FillVideoMode(ctx.r3.u32); }

// u32 VdQueryVideoFlags(): 1 = widescreen, 2 = >= 720p, 4 = >= 1080p
GUEST_IMPORT(VdQueryVideoFlags) { ctx.r3.u64 = 0x3; }

// void VdGetCurrentDisplayInformation(X_DISPLAY_INFO*)
GUEST_IMPORT(VdGetCurrentDisplayInformation)
{
    const uint32_t info = ctx.r3.u32;
    std::memset(Memory::Translate(info), 0, 0x58);
    Memory::Store16(info + 0x00, 1280); // front buffer width
    Memory::Store16(info + 0x02, 720);  // front buffer height
    // Scaler parameters: source and destination = full 1280x720 frame.
    Memory::Store32(info + 0x10, 1280);
    Memory::Store32(info + 0x14, 720);
    Memory::Store32(info + 0x20, 1280);
    Memory::Store32(info + 0x24, 720);
    Memory::Store16(info + 0x48, 1280); // display width
    Memory::Store16(info + 0x4A, 720);  // display height
    Memory::Store32(info + 0x4C, FloatBits(60));
}

// void VdGetCurrentDisplayGamma(u32* Type, float* Power)
GUEST_IMPORT(VdGetCurrentDisplayGamma)
{
    if (ctx.r3.u32) Memory::Store32(ctx.r3.u32, 2);
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, FloatBits(2.22222233f));
}

GUEST_IMPORT(VdSetDisplayMode)          { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdSetDisplayModeOverride)  { ctx.r3.u64 = 0; }

// u32 VdPersistDisplay(u32, u32* Unknown)
GUEST_IMPORT(VdPersistDisplay)
{
    if (ctx.r4.u32) Memory::Store32(ctx.r4.u32, 0);
    ctx.r3.u64 = 1;
}

GUEST_IMPORT(VdIsHSIOTrainingSucceeded)   { ctx.r3.u64 = 1; }
GUEST_IMPORT(VdRetrainEDRAM)              { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdRetrainEDRAMWorker)        { ctx.r3.u64 = 0; }
GUEST_IMPORT(VdEnableDisableClockGating)  { ctx.r3.u64 = 0; }
GUEST_IMPORT(KiApcNormalRoutineNop) {}

// ---------------------------------------------------------------------- swap
// u32 VdSwap(void* RingBufferSlot, fetch, unk2, unk3, unk4, u32* FrontBuffer,
//            u32* Format, u32* ColorSpace, u32* Width, u32* Height)
// The title reserves 64 dwords in the ring buffer for the kernel to fill; we emit a
// private XE_SWAP packet that the command processor turns into a frame boundary.
GUEST_IMPORT(VdSwap)
{
    // Packet: [0] header, [1] 'SWAP', [2] front buffer physical address, [3] width, [4] height,
    //         [5..10] the front buffer's texture fetch constant (r4 points at it), rest NOP.
    const uint32_t slot = ctx.r3.u32;
    const uint32_t fetch = ctx.r4.u32;
    const uint32_t frontBufferPtr = ctx.r8.u32;
    Memory::Store32(slot + 0x00, PacketType3(0x64, 63));
    Memory::Store32(slot + 0x04, 0x53574150); // 'SWAP'
    Memory::Store32(slot + 0x08, frontBufferPtr ? Memory::Load32(frontBufferPtr) & 0x1FFFFFFF : 0);
    Memory::Store32(slot + 0x0C, 1280);
    Memory::Store32(slot + 0x10, 720);
    for (uint32_t i = 0; i < 6; ++i)
        Memory::Store32(slot + 0x14 + i * 4, fetch ? Memory::Load32(fetch + i * 4) : 0);
    for (uint32_t i = 11; i < 64; ++i)
        Memory::Store32(slot + i * 4, 0x80000000);
    ctx.r3.u64 = 0;
}
