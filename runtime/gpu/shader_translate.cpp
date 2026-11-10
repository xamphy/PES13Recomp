#include "shader_translate.h"

#include "xenia/base/string_buffer.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/xenos.h"
#include "third_party/glslang/SPIRV/disassemble.h"

#include <bit>
#include <mutex>
#include <sstream>

namespace
{
    using xe::gpu::Shader;
    using xe::gpu::SpirvShaderTranslator;

    std::mutex g_mutex; // the translator object is not thread-safe

    SpirvShaderTranslator& Translator()
    {
        // Conservative feature set until the renderer passes its real Vulkan device features.
        static SpirvShaderTranslator::Features features(false);
        static SpirvShaderTranslator translator(features,
            /*native_2x_msaa_with_attachments=*/true,
            /*native_2x_msaa_no_attachments=*/true,
            /*edram_fragment_shader_interlock=*/false);
        return translator;
    }
}

namespace ShaderTranslate
{
    Result Translate(Stage stage, const uint32_t* dwords, size_t count)
    {
        Result result;
        std::lock_guard lock(g_mutex);
        const auto type = stage == Stage::Vertex ? xe::gpu::xenos::ShaderType::kVertex : xe::gpu::xenos::ShaderType::kPixel;
        xe::gpu::SpirvShader shader(type, 0, dwords, count, std::endian::native);

        xe::StringBuffer disasm;
        shader.AnalyzeUcode(disasm);
        result.ucodeDisassembly = shader.ucode_disassembly();

        SpirvShaderTranslator& translator = Translator();
        const uint64_t modification = stage == Stage::Vertex
            ? translator.GetDefaultVertexShaderModification(xe::gpu::xenos::kMaxShaderTempRegisters)
            : translator.GetDefaultPixelShaderModification(xe::gpu::xenos::kMaxShaderTempRegisters);
        Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
        if (!translator.TranslateAnalyzedShader(*translation) || !translation->is_valid())
        {
            result.error = "translation failed";
            return result;
        }
        const auto& binary = translation->translated_binary();
        result.spirv.resize(binary.size() / 4);
        std::memcpy(result.spirv.data(), binary.data(), result.spirv.size() * 4);
        result.ok = !result.spirv.empty();
        return result;
    }

    std::string DisassembleSpirv(const std::vector<uint32_t>& spirv)
    {
        std::ostringstream out;
        spv::Disassemble(out, spirv);
        return out.str();
    }
}

#include "xenia/gpu/draw_util.h"
namespace { [[maybe_unused]] auto* g_drawUtilProbe = &xe::gpu::draw_util::GetHostViewportInfo; [[maybe_unused]] auto* g_resolveProbe = &xe::gpu::draw_util::GetResolveInfo; }
