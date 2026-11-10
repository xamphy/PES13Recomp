#pragma once
// Xenos shader microcode -> SPIR-V, using the translator ported from Xenia
// (thirdparty/xenia, BSD-3). Runs at runtime the first time a shader is used.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ShaderTranslate
{
    enum class Stage { Vertex, Pixel };

    struct Result
    {
        bool ok = false;
        std::vector<uint32_t> spirv;   // SPIR-V words
        std::string ucodeDisassembly;  // Xenos microcode, human-readable
        std::string error;
    };

    // `dwords` are the microcode instruction words as logical values (as the command
    // processor reads them with Memory::Load32).
    Result Translate(Stage stage, const uint32_t* dwords, size_t count);

    // Human-readable SPIR-V (glslang's disassembler), for debugging.
    std::string DisassembleSpirv(const std::vector<uint32_t>& spirv);
}
