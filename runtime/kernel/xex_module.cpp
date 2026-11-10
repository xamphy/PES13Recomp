#include "xex_module.h"
#include "kernel.h"
#include "kernel_vars.h"

#include <guest_memory.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    Kernel::ModuleInfo g_module;

    uint32_t FileU32(const std::vector<uint8_t>& f, size_t offset)
    {
        return (uint32_t(f[offset]) << 24) | (uint32_t(f[offset + 1]) << 16) | (uint32_t(f[offset + 2]) << 8) | f[offset + 3];
    }

    uint16_t FileU16(const std::vector<uint8_t>& f, size_t offset)
    {
        return uint16_t((f[offset] << 8) | f[offset + 1]);
    }

    // Walks the import descriptor (key 0x000103FF) in the raw file and calls
    // onRecord(libraryName, slotAddress) for every import record.
    template<typename F>
    bool ForEachImportRecord(const std::vector<uint8_t>& f, F&& onRecord)
    {
        const uint32_t count = FileU32(f, 0x14);
        uint32_t desc = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (FileU32(f, 0x18 + i * 8) == Kernel::XEX_HEADER_IMPORT_LIBRARIES)
                desc = FileU32(f, 0x18 + i * 8 + 4);
        if (desc == 0)
            return false;

        const uint32_t stringTableSize = FileU32(f, desc + 4);
        const uint32_t libraryCount = FileU32(f, desc + 8);

        std::vector<const char*> names;
        for (size_t p = desc + 12; p < desc + 12 + stringTableSize;)
        {
            const char* s = reinterpret_cast<const char*>(&f[p]);
            if (*s) names.push_back(s);
            p += std::strlen(s) + 1;
            while (p < desc + 12 + stringTableSize && f[p] == 0) ++p; // padding
        }

        size_t lib = desc + 12 + stringTableSize;
        for (uint32_t l = 0; l < libraryCount; ++l)
        {
            const uint32_t size = FileU32(f, lib);
            const uint16_t nameIndex = FileU16(f, lib + 0x24);
            const uint16_t recordCount = FileU16(f, lib + 0x26);
            const char* name = nameIndex < names.size() ? names[nameIndex] : "?";
            for (uint16_t r = 0; r < recordCount; ++r)
                onRecord(name, FileU32(f, lib + 0x28 + r * 4));
            lib += size;
        }
        return true;
    }

    // X_LDR_DATA_TABLE_ENTRY (subset). Offsets follow Xenia's layout.
    uint32_t CreateModuleObject(const Kernel::ModuleInfo& m)
    {
        const uint32_t entry = Memory::AllocKernel(0x100);
        // The three list heads point at themselves (empty lists).
        for (uint32_t list = 0; list < 0x18; list += 8)
        {
            Memory::Store32(entry + list + 0, entry + list);
            Memory::Store32(entry + list + 4, entry + list);
        }
        Memory::Store32(entry + 0x18, m.imageBase);   // DllBase
        Memory::Store32(entry + 0x1C, m.entryPoint);  // EntryPoint
        Memory::Store32(entry + 0x20, m.imageSize);   // SizeOfImage
        Memory::Store32(entry + 0x38, m.imageSize);   // FullImageSize
        Memory::Store16(entry + 0x40, 1);             // LoadCount
        Memory::Store32(entry + 0x44, m.imageBase);   // DllBaseOriginal
        Memory::Store32(entry + 0x58, m.headerAddr);  // XexHeaderBase
        return entry;
    }
}

namespace Kernel
{
    const ModuleInfo& ExecutableModule() { return g_module; }

    uint32_t GetXexHeaderField(uint32_t headerAddr, uint32_t key)
    {
        if (headerAddr == 0)
            return 0;
        const uint32_t count = Memory::Load32(headerAddr + 0x14);
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t entry = headerAddr + 0x18 + i * 8;
            if (Memory::Load32(entry) != key)
                continue;
            const uint32_t value = Memory::Load32(entry + 4);
            return (key & 0xFF) == 0x00 ? value : headerAddr + value;
        }
        return 0;
    }

    bool LoadExecutableModule(const std::vector<uint8_t>& xex, uint32_t imageBase, uint32_t imageSize,
        uint32_t entryPoint, ModuleInfo& out)
    {
        ModuleInfo m;
        m.imageBase = imageBase;
        m.imageSize = imageSize;
        m.entryPoint = entryPoint;

        // 1. Full XEX header (optional headers, security info, import descriptor...).
        m.headerSize = FileU32(xex, 0x08);
        if (m.headerSize == 0 || m.headerSize > xex.size())
            return false;
        m.headerAddr = Memory::AllocKernel(m.headerSize, 0x1000);
        std::memcpy(Memory::Translate(m.headerAddr), xex.data(), m.headerSize);

        // 2. Thread defaults.
        if (const uint32_t stack = GetXexHeaderField(m.headerAddr, XEX_HEADER_DEFAULT_STACK_SIZE))
            m.stackSize = stack;
        if (const uint32_t tls = GetXexHeaderField(m.headerAddr, XEX_HEADER_TLS_INFO))
        {
            m.tlsSlotCount   = Memory::Load32(tls + 0x0);
            m.tlsRawDataAddr = Memory::Load32(tls + 0x4);
            m.tlsDataSize    = Memory::Load32(tls + 0x8);
            m.tlsRawDataSize = Memory::Load32(tls + 0xC);
        }

        // 3. Module object.
        m.moduleHandle = CreateModuleObject(m);
        g_module = m;

        // 4. Kernel variable imports. Function import records are left alone: the
        //    recompiler already replaced the thunks with direct calls to __imp__ functions.
        uint32_t resolved = 0, unknown = 0;
        const bool ok = ForEachImportRecord(xex, [&](const char* library, uint32_t slot)
        {
            // XenonUtils' Xex2LoadImage byte-swaps every import record in place (and
            // patches thunk records into nops), so the slot holds the record in host order.
            const uint32_t record = *Memory::Translate<uint32_t>(slot);
            if ((record >> 24) != 0 || std::strcmp(library, "xboxkrnl.exe") != 0)
                return; // thunk record, or a xam import (xam exports no variables we use)

            const uint16_t ordinal = record & 0xFFFF;
            if (!IsKernelVariable(ordinal))
                return; // first record of a function import

            const uint32_t addr = ResolveKernelVariable(ordinal);
            if (addr)
            {
                Memory::Store32(slot, addr);
                ++resolved;
            }
            else
            {
                ++unknown;
            }
        });

        std::printf("  module     : header 0x%08X (%u bytes), handle 0x%08X\n", m.headerAddr, m.headerSize, m.moduleHandle);
        std::printf("  stack/TLS  : stack 0x%X, %u TLS slots, %u bytes static TLS\n", m.stackSize, m.tlsSlotCount, m.tlsDataSize);
        std::printf("  imports    : %u kernel variables resolved%s\n", resolved, unknown ? " (some unknown - see log)" : "");

        out = m;
        return ok;
    }
}

GUEST_IMPORT(RtlImageXexHeaderField)
{
    ctx.r3.u64 = Kernel::GetXexHeaderField(ctx.r3.u32, ctx.r4.u32);
}

// ---------------------------------------------------------------------------
// Module lookup: the executable plus the two system modules titles link against.
// ---------------------------------------------------------------------------
namespace
{
    constexpr uint32_t STATUS_SUCCESS            = 0x00000000;
    constexpr uint32_t STATUS_NO_SUCH_FILE       = 0xC000000F;
    constexpr uint32_t STATUS_DLL_NOT_FOUND      = 0xC0000135;
    constexpr uint32_t STATUS_ORDINAL_NOT_FOUND  = 0xC0000138;

    uint32_t g_xamModule = 0;
    uint32_t g_kernelModule = 0;

    std::string GuestString(uint32_t addr)
    {
        std::string s;
        if (addr)
            for (char c; (c = char(Memory::Load8(addr))) != 0 && s.size() < 260; ++addr)
                s.push_back(c);
        return s;
    }

    bool EndsWithNoCase(const std::string& s, const char* suffix)
    {
        const size_t n = std::strlen(suffix);
        if (s.size() < n)
            return false;
        for (size_t i = 0; i < n; ++i)
            if (std::tolower((unsigned char)s[s.size() - n + i]) != std::tolower((unsigned char)suffix[i]))
                return false;
        return true;
    }

    // Returns the module handle for a module name (empty = the executable), or 0.
    uint32_t FindModule(const std::string& name)
    {
        const Kernel::ModuleInfo& exe = Kernel::ExecutableModule();
        if (name.empty() || EndsWithNoCase(name, "default.xex"))
            return exe.moduleHandle;

        auto makeSystemModule = [](uint32_t& handle)
        {
            if (!handle)
            {
                Kernel::ModuleInfo sys; // no image or header: titles only use the handle for lookups
                handle = CreateModuleObject(sys);
            }
            return handle;
        };
        if (EndsWithNoCase(name, "xam.xex"))
            return makeSystemModule(g_xamModule);
        if (EndsWithNoCase(name, "xboxkrnl.exe"))
            return makeSystemModule(g_kernelModule);
        return 0;
    }

    const char* ModuleName(uint32_t handle)
    {
        if (handle == g_xamModule) return "xam.xex";
        if (handle == g_kernelModule) return "xboxkrnl.exe";
        if (handle == Kernel::ExecutableModule().moduleHandle) return "default.xex";
        return "?";
    }
}

// NTSTATUS XexGetModuleHandle(const char* ModuleName, u32* HandlePtr)
GUEST_IMPORT(XexGetModuleHandle)
{
    const std::string name = GuestString(ctx.r3.u32);
    const uint32_t handle = FindModule(name);
    if (ctx.r4.u32)
        Memory::Store32(ctx.r4.u32, handle);
    ctx.r3.u64 = handle ? STATUS_SUCCESS : STATUS_DLL_NOT_FOUND;
}

// NTSTATUS XexLoadImage(const char* Name, u32 Flags, u32 MinVersion, u32* HandlePtr)
GUEST_IMPORT(XexLoadImage)
{
    const std::string name = GuestString(ctx.r3.u32);
    const uint32_t handle = FindModule(name);
    if (ctx.r6.u32)
        Memory::Store32(ctx.r6.u32, handle);
    if (!handle)
        std::fprintf(stderr, "[xex] XexLoadImage(\"%s\") - module not available\n", name.c_str());
    ctx.r3.u64 = handle ? STATUS_SUCCESS : STATUS_NO_SUCH_FILE;
}

// NTSTATUS XexUnloadImage(u32 Handle)
GUEST_IMPORT(XexUnloadImage)
{
    ctx.r3.u64 = STATUS_SUCCESS; // modules are never actually unloaded
}

// NTSTATUS XexGetProcedureAddress(u32 Handle, u32 Ordinal, u32* ProcAddressPtr)
//
// Dynamically resolved exports are optional features (e.g. XamPartyGetUserList).
// Recompiled code can only call guest addresses inside the executable, so we report
// "not found" - the same thing an older system software version would do - and log
// what was asked for, in case a title turns out to depend on one.
GUEST_IMPORT(XexGetProcedureAddress)
{
    const uint32_t handle = ctx.r3.u32, ordinal = ctx.r4.u32, outPtr = ctx.r5.u32;
    if (outPtr)
        Memory::Store32(outPtr, 0);
    std::fprintf(stderr, "[xex] XexGetProcedureAddress(%s, ordinal 0x%X) - not provided\n", ModuleName(handle), ordinal);
    ctx.r3.u64 = STATUS_ORDINAL_NOT_FOUND;
}

// u32 KeGetCurrentProcessType(): 1 = title (user) process.
GUEST_IMPORT(KeGetCurrentProcessType)
{
    ctx.r3.u64 = 1;
}
