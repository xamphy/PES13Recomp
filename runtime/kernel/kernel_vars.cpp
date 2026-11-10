// Kernel variable exports (the "kVariable" entries of xboxkrnl_table.inc).
//
// On a console these are kernel data structures the loader links into the
// title's import table. Values follow what Xenia exposes to titles.
#include "kernel_vars.h"
#include "xex_module.h"

#include <guest_memory.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unordered_map>

namespace
{
    enum ExportKind { kFunction, kVariable };

    struct ExportInfo
    {
        const char* name;
        ExportKind kind;
    };

    const std::unordered_map<uint16_t, ExportInfo>& KernelExports()
    {
        static const std::unordered_map<uint16_t, ExportInfo> table = {
#define XE_EXPORT(MODULE, ORDINAL, NAME, KIND) { uint16_t(ORDINAL), { #NAME, KIND } } // each table line supplies its own trailing comma
#include <xbox/xboxkrnl_table.inc>
#undef XE_EXPORT
        };
        return table;
    }

    std::unordered_map<uint16_t, uint32_t> g_resolved;
    uint32_t g_timeStampBundle = 0;

    uint32_t AllocString(const char* s, uint32_t capacity)
    {
        const uint32_t addr = Memory::AllocKernel(capacity);
        std::strncpy(Memory::Translate<char>(addr), s, capacity - 1);
        return addr;
    }

    // 100 ns units since 1601-01-01 (Windows FILETIME), as the kernel reports system time.
    uint64_t HostFileTime()
    {
        using namespace std::chrono;
        constexpr uint64_t UnixEpochAsFileTime = 116444736000000000ull;
        return UnixEpochAsFileTime + duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100;
    }

    uint32_t CreateVariable(uint16_t ordinal, const char* name)
    {
        switch (ordinal)
        {
        case 0x156: // XboxHardwareInfo { u32 Flags; u8 NumberOfProcessors; u8 PCIBridgeRevision; u8 Reserved[6]; u16 BldrMagic; u16 BldrFlags; }
        {
            const uint32_t v = Memory::AllocKernel(0x10);
            Memory::Store32(v + 0, 0x00000020); // HDD present
            Memory::Store8(v + 4, 6);           // 3 cores x 2 hardware threads
            return v;
        }
        case 0x157: // XboxKrnlBaseVersion
        case 0x158: // XboxKrnlVersion { u16 Major, Minor, Build, Qfe } - 2.0.17559.0
        {
            const uint32_t v = Memory::AllocKernel(8);
            Memory::Store16(v + 0, 2);
            Memory::Store16(v + 2, 0);
            Memory::Store16(v + 4, 17559);
            Memory::Store16(v + 6, 0);
            return v;
        }
        case 0x193: // XexExecutableModuleHandle: pointer to the module's LDR_DATA_TABLE_ENTRY
        {
            const uint32_t v = Memory::AllocKernel(4);
            Memory::Store32(v, Kernel::ExecutableModule().moduleHandle);
            return v;
        }
        case 0x1AE: // ExLoadedCommandLine (char[])
            return AllocString("\"default.xex\"", 0x400);
        case 0x1AF: // ExLoadedImageName (char[])
            return AllocString("\\Device\\Cdrom0\\default.xex", 0x100);
        case 0x0AD: // KeTimeStampBundle { u64 InterruptTime; u64 SystemTime; u32 TickCount; u32 pad; }
        {
            g_timeStampBundle = Memory::AllocKernel(0x18);
            Memory::Store64(g_timeStampBundle + 8, HostFileTime());
            return g_timeStampBundle;
        }
        case 0x1C0: // VdGpuClockInMHz
        {
            const uint32_t v = Memory::AllocKernel(4);
            Memory::Store32(v, 500);
            return v;
        }
        case 0x1BE: // VdGlobalDevice     (pointer, set by the title's D3D)
        case 0x1BF: // VdGlobalXamDevice
            return Memory::AllocKernel(4);
        case 0x1C1: // VdHSIOCalibrationLock (RTL_CRITICAL_SECTION, 0x1C bytes)
            return Memory::AllocKernel(0x1C);
        case 0x059: // KeDebugMonitorData
        case 0x266: // KeCertMonitorData
            return Memory::AllocKernel(0x100);
        case 0x00C: // ExConsoleGameRegion
        {
            const uint32_t v = Memory::AllocKernel(4);
            Memory::Store32(v, 0x00FF); // all regions
            return v;
        }
        default:
            // Object types (Ex*ObjectType, Io*ObjectType, Ob*ObjectType) and anything
            // else: a zeroed block. Titles generally only compare these by address.
            std::printf("[kernel] variable %s (0x%03X) -> zeroed placeholder\n", name, ordinal);
            return Memory::AllocKernel(0x100);
        }
    }
}

namespace Kernel
{
    bool IsKernelVariable(uint16_t ordinal)
    {
        const auto it = KernelExports().find(ordinal);
        return it != KernelExports().end() && it->second.kind == kVariable;
    }

    uint32_t ResolveKernelVariable(uint16_t ordinal)
    {
        if (const auto it = g_resolved.find(ordinal); it != g_resolved.end())
            return it->second;

        const auto info = KernelExports().find(ordinal);
        if (info == KernelExports().end() || info->second.kind != kVariable)
            return 0;

        const uint32_t addr = CreateVariable(ordinal, info->second.name);
        g_resolved[ordinal] = addr;
        return addr;
    }

    void StartTimeStampUpdater()
    {
        if (!g_timeStampBundle)
            return;
        std::thread([]
        {
            const auto start = std::chrono::steady_clock::now();
            const uint64_t startFileTime = HostFileTime();
            for (;;)
            {
                const auto elapsed = std::chrono::steady_clock::now() - start;
                const uint64_t ticks100ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / 100;
                Memory::Store64(g_timeStampBundle + 0x00, ticks100ns);                 // InterruptTime
                Memory::Store64(g_timeStampBundle + 0x08, startFileTime + ticks100ns); // SystemTime
                Memory::Store32(g_timeStampBundle + 0x10, uint32_t(ticks100ns / 10000)); // TickCount (ms)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }).detach();
    }
}
