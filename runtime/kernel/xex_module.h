#pragma once
#include <cstdint>
#include <vector>

namespace Kernel
{
    // XEX optional-header keys we use. Keys whose low byte is 0x00 store their
    // value inline; for 0x01 the value is a single dword; otherwise it is an
    // offset (from the start of the header) to a block of (low byte * 4) bytes.
    enum : uint32_t
    {
        XEX_HEADER_ENTRY_POINT        = 0x00010100,
        XEX_HEADER_IMAGE_BASE_ADDRESS = 0x00010201,
        XEX_HEADER_IMPORT_LIBRARIES   = 0x000103FF,
        XEX_HEADER_TLS_INFO           = 0x00020104,
        XEX_HEADER_DEFAULT_STACK_SIZE = 0x00020200,
    };

    struct ModuleInfo
    {
        uint32_t headerAddr = 0;    // guest copy of the full XEX header
        uint32_t headerSize = 0;
        uint32_t moduleHandle = 0;  // guest LDR_DATA_TABLE_ENTRY (what XexExecutableModuleHandle points at)
        uint32_t imageBase = 0;
        uint32_t imageSize = 0;
        uint32_t entryPoint = 0;
        uint32_t stackSize = 0x40000;

        // XEX_HEADER_TLS_INFO
        uint32_t tlsSlotCount = 64;
        uint32_t tlsRawDataAddr = 0;
        uint32_t tlsDataSize = 0;
        uint32_t tlsRawDataSize = 0;
    };

    // Copies the XEX header into guest memory, creates the module object and
    // resolves every kernel *variable* import (function imports are handled by
    // the recompiled thunks). The image must already be in guest memory.
    bool LoadExecutableModule(const std::vector<uint8_t>& xexFile, uint32_t imageBase, uint32_t imageSize,
        uint32_t entryPoint, ModuleInfo& out);

    const ModuleInfo& ExecutableModule();

    // RtlImageXexHeaderField semantics: inline value for keys ending in 0x00,
    // otherwise the guest address of the field data. 0 if the key is absent.
    uint32_t GetXexHeaderField(uint32_t headerAddr, uint32_t key);
}
