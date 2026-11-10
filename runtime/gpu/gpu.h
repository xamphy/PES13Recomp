#pragma once
// Xenos GPU front end: MMIO register file, command processor (PM4 ring buffer)
// and graphics interrupts. No rendering yet - draws are decoded and counted only.
#include <cstdint>

namespace Gpu
{
    // GPU registers live at guest 0x7FC80000 + index * 4 (big-endian), so the
    // title's MMIO loads/stores hit plain memory. Must be called after Memory::Init.
    void Init();

    uint32_t ReadRegister(uint32_t index);
    void WriteRegister(uint32_t index, uint32_t value);

    // Physical (GPU) address -> guest virtual address of the 0xA0000000 view.
    inline uint32_t PhysicalToGuest(uint32_t physical) { return 0xA0000000u | (physical & 0x1FFFFFFFu); }

    // From the Vd* imports.
    void InitializeRingBuffer(uint32_t physicalAddress, uint32_t sizeLog2);
    void EnableReadPointerWriteBack(uint32_t physicalAddress, uint32_t blockSizeLog2);
    void SetInterruptCallback(uint32_t callback, uint32_t userData);

    // Statistics for logging.
    uint64_t FrameCount();
    void DumpFrame(uint64_t frame); // --dump-frame N: print every draw of frame N with its state
    void DumpState(); // watchdog report
    void SetCpSpin(bool spin); // control "cp spin|sleep" (PES_CP_SPIN at start)
}

// Register indices used outside the command processor.
namespace GpuReg
{
    constexpr uint32_t CP_RB_RPTR = 0x01C4;
    constexpr uint32_t CP_RB_WPTR = 0x01C5;
    constexpr uint32_t SCRATCH_UMSK = 0x01DC;
    constexpr uint32_t SCRATCH_ADDR = 0x01DD;
    constexpr uint32_t SCRATCH_REG0 = 0x0578;
    constexpr uint32_t RB_EDRAM_TIMING = 0x0F00;
    constexpr uint32_t RB_BC_CONTROL = 0x0F01;
    constexpr uint32_t D1MODE_V_COUNTER = 0x194C;
    constexpr uint32_t D1MODE_INTERRUPT_STATUS = 0x1951;
    constexpr uint32_t D1MODE_VIEWPORT_SIZE = 0x1961;
    constexpr uint32_t VGT_EVENT_INITIATOR = 0x21F9;
}
