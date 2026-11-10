#pragma once
// CPU writes to memory that holds GPU resolve results.
//
// The renderer keeps resolve results on the GPU ("surfaces"). When the game later puts other
// data in that memory (PES reuses pools for glyphs, names, numbers, kits), the surface is
// stale and the texture must be read from guest memory instead. A sparse snapshot cannot see
// every such write, so the pages of every resolve destination are write-protected in all three
// physical views (0xA0000000 / 0xC0000000 / 0xE0000000); the first write to such a page - by
// guest code or by host code - is recorded (page + write epoch) and the page is unprotected.
#include <cstdint>

namespace MemoryWatch
{
    void Init();

    // Write-protects the pages covering [physical, physical + size).
    void Protect(uint32_t physical, uint32_t size);

    // Monotonic counter, bumped by every recorded write.
    uint32_t Epoch();

    // Writes recorded so far (protected-page faults; performance statistics).
    uint64_t Faults();

    // True if any page of [physical, physical + size) was written after `epoch`.
    bool WrittenSince(uint32_t physical, uint32_t size, uint32_t epoch);

    // Host code is about to write [guestAddr, guestAddr + size) in a way that cannot fault
    // (kernel-mode writes such as ReadFile): unprotects and records the pages. Any guest
    // address; only the physical views are tracked.
    void HostWillWrite(uint32_t guestAddr, uint32_t size);

    // Debug: report (once) the guest code of the next write into [physical, physical + size).
    void ReportNextWrite(uint32_t physical, uint32_t size);
    // Debug: report the guest code writing a virtual (non-physical) guest range.
    void ReportVirtualWrites(uint32_t guestAddr, uint32_t size);
    // Debug: report the first read or write (any view) of [physical, physical + size).
    void ReportAccess(uint32_t physical, uint32_t size);

    // Writes [physical, physical + size) without recording it as a write (the GPU copy of that
    // memory stays valid): the renderer storing a CPU view of a resolve.
    void UntrackedWrite(uint32_t physical, uint32_t size, void (*write)(uint8_t* p, uint32_t size, void* user), void* user);
}
