// Shaders and pipelines: modules, layouts, blend state, pipeline cache, constants, descriptor sets.
// Part of the renderer (see renderer_internal.h for the file map).
#include "renderer_internal.h"

namespace RendererImpl
{
    VkShaderModule GetModule(ShaderEntry& e, uint64_t modification)
    {
        auto it = e.modules.find(modification);
        if (it != e.modules.end()) return it->second;
        ScopedTimer timer{ g_time.modules };
        ScopedTimer total{ g_compileUsTotal };
        ++g_time.newModules;
        ++g_modulesTotal;
        VkShaderModule module = VK_NULL_HANDLE;
        Shader::Translation* t = e.shader->GetOrCreateTranslation(modification);
        if (!t->is_translated())
            g_translator->TranslateAnalyzedShader(*t);
        if (t->is_valid() && !t->translated_binary().empty())
        {
            VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
            ci.codeSize = t->translated_binary().size();
            ci.pCode = reinterpret_cast<const uint32_t*>(t->translated_binary().data());
            if (vkCreateShaderModule(g_dev, &ci, nullptr, &module) != VK_SUCCESS)
                module = VK_NULL_HANDLE;
            static const bool dumpModules = std::getenv("PES_DUMP_MODULES") != nullptr;
            if (dumpModules)
            {
                std::vector<uint32_t> words(t->translated_binary().size() / 4);
                std::memcpy(words.data(), t->translated_binary().data(), words.size() * 4);
                std::ostringstream text;
                spv::Disassemble(text, words);
                char name[128];
                std::snprintf(name, sizeof(name), "userdata/shader_dump/%016llX_%016llX.%s.module.txt",
                    (unsigned long long)e.shader->ucode_data_hash(), (unsigned long long)modification,
                    e.shader->type() == xenos::ShaderType::kVertex ? "vs" : "ps");
                if (FILE* f = std::fopen(name, "wb")) { std::fputs(text.str().c_str(), f); std::fclose(f); }
            }
        }
        if (!module)
        {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "shader %016llX failed to translate", (unsigned long long)e.shader->ucode_data_hash());
            WarnOnce(msg);
        }
        e.modules.emplace(modification, module);
        return module;
    }

    void SavePipelineCache()
    {
        if (!g_pipelineCacheDirty || !g_pipelineCache) return;
        size_t size = 0;
        if (vkGetPipelineCacheData(g_dev, g_pipelineCache, &size, nullptr) != VK_SUCCESS || !size) return;
        std::vector<uint8_t> data(size);
        if (vkGetPipelineCacheData(g_dev, g_pipelineCache, &size, data.data()) != VK_SUCCESS) return;
        if (FILE* f = std::fopen(kPipelineCachePath, "wb"))
        {
            std::fwrite(data.data(), 1, size, f);
            std::fclose(f);
        }
        g_pipelineCacheDirty = false;
    }

    VkDescriptorSetLayout TextureLayout(bool pixel, uint32_t textures, uint32_t samplers)
    {
        if (!textures && !samplers) return g_layoutEmpty;
        auto& slot = g_textureLayouts[pixel][{ textures, samplers }];
        if (slot) return slot;
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        const VkShaderStageFlags stages = pixel ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        for (uint32_t i = 0; i < textures + samplers; ++i)
            bindings.push_back({ i, i < textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER, 1, stages, nullptr });
        VkDescriptorSetLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        ci.bindingCount = uint32_t(bindings.size());
        ci.pBindings = bindings.data();
        vkCreateDescriptorSetLayout(g_dev, &ci, nullptr, &slot);
        return slot;
    }

    PipelineLayout* GetPipelineLayout(uint32_t tv, uint32_t sv, uint32_t tp, uint32_t sp)
    {
        auto& pl = g_pipelineLayouts[{ tv, sv, tp, sp }];
        if (pl.layout) return &pl;
        pl.textures[0] = TextureLayout(false, tv, sv);
        pl.textures[1] = TextureLayout(true, tp, sp);
        VkDescriptorSetLayout sets[Translator::kDescriptorSetCount];
        sets[Translator::kDescriptorSetSharedMemoryAndEdram] = g_layoutShared;
        sets[Translator::kDescriptorSetConstants] = g_layoutConstants;
        sets[Translator::kDescriptorSetTexturesVertex] = pl.textures[0];
        sets[Translator::kDescriptorSetTexturesPixel] = pl.textures[1];
        VkPipelineLayoutCreateInfo ci{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        ci.setLayoutCount = Translator::kDescriptorSetCount;
        ci.pSetLayouts = sets;
        vkCreatePipelineLayout(g_dev, &ci, nullptr, &pl.layout);
        return &pl;
    }

    VkBlendFactor BlendFactor(uint32_t f)
    {
        switch (f)
        {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 4: return VK_BLEND_FACTOR_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_DST_ALPHA;
        case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
        }
    }

    VkBlendOp BlendOp(uint32_t op)
    {
        switch (op)
        {
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
        }
    }

    VkPipeline GetPipeline(const PipelineDesc& d)
    {
        const std::vector<uint64_t> key = { uint64_t(d.vs), uint64_t(d.ps), uint64_t(d.topology), d.primitiveRestart, d.cull,
            uint64_t(d.frontFace), d.colorMask, d.blend, uint64_t(d.format), uint64_t(d.layout->layout), uint64_t(d.depthFormat),
            d.depthControl, d.depthClamp };
        auto it = g_pipelines.find(key);
        if (it != g_pipelines.end()) return it->second;
        ScopedTimer timer{ g_time.pipelines };
        ScopedTimer total{ g_compileUsTotal };
        ++g_time.newPipelines;
        ++g_pipelinesTotal;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, d.vs, "main" };
        stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, d.ps, "main" };
        VkPipelineVertexInputStateCreateInfo vi{ VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia{ VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = d.topology;
        ia.primitiveRestartEnable = d.primitiveRestart;
        VkPipelineViewportStateCreateInfo vp{ VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{ VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = d.cull;
        rs.frontFace = d.frontFace;
        rs.lineWidth = 1.0f;
        rs.depthClampEnable = d.depthClamp && g_vk->enabledFeatures.depthClamp;
        VkPipelineMultisampleStateCreateInfo ms{ VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{ VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
        if (d.depthFormat != VK_FORMAT_UNDEFINED)
        {
            // Xenos CompareFunction and StencilOp use Vulkan's orderings.
            reg::RB_DEPTHCONTROL dc;
            dc.value = d.depthControl;
            ds.depthTestEnable = dc.z_enable;
            ds.depthWriteEnable = dc.z_write_enable;
            ds.depthCompareOp = VkCompareOp(dc.zfunc);
            ds.stencilTestEnable = dc.stencil_enable;
            ds.front.failOp = VkStencilOp(dc.stencilfail);
            ds.front.passOp = VkStencilOp(dc.stencilzpass);
            ds.front.depthFailOp = VkStencilOp(dc.stencilzfail);
            ds.front.compareOp = VkCompareOp(dc.stencilfunc);
            if (dc.backface_enable)
            {
                ds.back.failOp = VkStencilOp(dc.stencilfail_bf);
                ds.back.passOp = VkStencilOp(dc.stencilzpass_bf);
                ds.back.depthFailOp = VkStencilOp(dc.stencilzfail_bf);
                ds.back.compareOp = VkCompareOp(dc.stencilfunc_bf);
            }
            else
                ds.back = ds.front;
        }
        VkPipelineColorBlendAttachmentState att{};
        att.colorWriteMask = d.colorMask;
        if ((d.blend & 0x1FFF1FFF) != 0x00010001)
        {
            att.blendEnable = VK_TRUE;
            att.srcColorBlendFactor = BlendFactor(d.blend & 0x1F);
            att.colorBlendOp = BlendOp((d.blend >> 5) & 7);
            att.dstColorBlendFactor = BlendFactor((d.blend >> 8) & 0x1F);
            att.srcAlphaBlendFactor = BlendFactor((d.blend >> 16) & 0x1F);
            att.alphaBlendOp = BlendOp((d.blend >> 21) & 7);
            att.dstAlphaBlendFactor = BlendFactor((d.blend >> 24) & 0x1F);
        }
        VkPipelineColorBlendStateCreateInfo cb{ VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
        cb.attachmentCount = 1;
        cb.pAttachments = &att;
        const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
            VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK };
        VkPipelineDynamicStateCreateInfo dy{ VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
        dy.dynamicStateCount = 6;
        dy.pDynamicStates = dyn;

        VkGraphicsPipelineCreateInfo ci{ VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        ci.stageCount = d.ps ? 2 : 1;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &ds;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &dy;
        ci.layout = d.layout->layout;
        ci.renderPass = RenderPassFor(d.format, d.depthFormat);
        VkPipeline pipeline = VK_NULL_HANDLE;
        g_pipelineCacheDirty = true;
        if (vkCreateGraphicsPipelines(g_dev, g_pipelineCache, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
        {
            WarnOnce("graphics pipeline creation failed");
            pipeline = VK_NULL_HANDLE;
        }
        g_pipelines.emplace(key, pipeline);
        return pipeline;
    }

    // ------------------------------------------------------------------ primitives
    // Host topology for a guest primitive type; fans and quads are converted to lists.
    bool HostTopology(xenos::PrimitiveType p, VkPrimitiveTopology& topology, bool& convert)
    {
        convert = false;
        switch (p)
        {
        case xenos::PrimitiveType::kLineList: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; return true;
        case xenos::PrimitiveType::kLineStrip: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; return true;
        case xenos::PrimitiveType::kTriangleList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; return true;
        case xenos::PrimitiveType::kTriangleStrip: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; return true;
        case xenos::PrimitiveType::kTriangleFan:
        case xenos::PrimitiveType::kQuadList:
        case xenos::PrimitiveType::kRectangleList: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; convert = true; return true;
        default: return false;
        }
    }

    // ------------------------------------------------------------------ system constants
    void FillSystemConstants(Translator::SystemConstants& sc, bool polygonal, const draw_util::ViewportInfo& vi,
        uint32_t usedTextureMask)
    {
        std::memset(&sc, 0, sizeof(sc));
        const auto vte = g_regs.Get<reg::PA_CL_VTE_CNTL>();
        const auto colorControl = g_regs.Get<reg::RB_COLORCONTROL>();
        const auto depthInfo = g_regs.Get<reg::RB_DEPTH_INFO>();
        const auto surfaceInfo = g_regs.Get<reg::RB_SURFACE_INFO>();

        uint32_t flags = 0;
        if (vte.vtx_xy_fmt) flags |= Translator::kSysFlag_XYDividedByW;
        if (vte.vtx_z_fmt) flags |= Translator::kSysFlag_ZDividedByW;
        if (vte.vtx_w0_fmt) flags |= Translator::kSysFlag_WNotReciprocal;
        if (polygonal) flags |= Translator::kSysFlag_PrimitivePolygonal;
        if (draw_util::IsPrimitiveLine(g_regs)) flags |= Translator::kSysFlag_PrimitiveLine;
        flags |= uint32_t(surfaceInfo.msaa_samples) << Translator::kSysFlag_MsaaSamples_Shift;
        if (depthInfo.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) flags |= Translator::kSysFlag_DepthFloat24;
        static const bool debugNoAlpha = std::getenv("PES_DEBUG_NOALPHA") != nullptr;
        const auto alphaFunc = colorControl.alpha_test_enable && !debugNoAlpha ? colorControl.alpha_func : xenos::CompareFunction::kAlways;
        flags |= uint32_t(alphaFunc) << Translator::kSysFlag_AlphaPassIfLess_Shift;
        for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i)
        {
            const auto ci = g_regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
            if (ci.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA)
                flags |= Translator::kSysFlag_ConvertColor0ToGamma << i;
            int32_t bias = ci.color_exp_bias;
            float scale;
            const int32_t bits = int32_t(0x3F800000) + (bias << 23);
            std::memcpy(&scale, &bits, 4);
            sc.color_exp_bias[i] = scale;
        }
        sc.flags = flags;
        sc.vertex_index_endian = xenos::Endian::kNone; // indices are converted on the host
        sc.vertex_base_index = g_regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
        for (int i = 0; i < 3; ++i)
        {
            sc.ndc_scale[i] = vi.ndc_scale[i];
            sc.ndc_offset[i] = vi.ndc_offset[i];
        }
        for (uint32_t t = 0; t < 32; ++t)
            if (usedTextureMask & (1u << t))
                sc.texture_swizzled_signs[t >> 2] |= uint32_t(texture_util::SwizzleSigns(g_regs.GetTextureFetch(t))) << (8 * (t & 3));
        sc.alpha_test_reference = g_regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
    }

    VkDescriptorBufferInfo UploadUniform(const void* data, size_t size)
    {
        VkDescriptorBufferInfo info{ g_uniforms.buf.buffer, 0, std::max<VkDeviceSize>(size, 16) };
        if (uint8_t* p = g_uniforms.Allocate(info.range, g_vk->properties.limits.minUniformBufferOffsetAlignment, info.offset))
        {
            std::memset(p, 0, info.range);
            std::memcpy(p, data, size);
        }
        return info;
    }

    // Float constants a shader uses, packed in register order straight into the uniform buffer.
    // The range is rounded up to a power of two so that dynamic constant sets can be shared by
    // all draws with the same (vertex, pixel) range bucket.
    VkDescriptorBufferInfo UploadFloatConstants(const Shader* shader, uint32_t baseRegister)
    {
        const uint32_t count = shader ? std::max<uint32_t>(shader->constant_register_map().float_count, 1) : 1;
        VkDeviceSize range = 16;
        while (range < VkDeviceSize(count) * 16) range <<= 1;
        VkDescriptorBufferInfo info{ g_uniforms.buf.buffer, 0, range };
        uint8_t* p = g_uniforms.Allocate(range, g_vk->properties.limits.minUniformBufferOffsetAlignment, info.offset);
        if (!p) return info;
        uint8_t* const end = p + range;
        if (shader)
        {
            const auto& map = shader->constant_register_map();
            for (uint32_t i = 0; i < 4; ++i)
                for (uint64_t bits = map.float_bitmap[i]; bits; bits &= bits - 1)
                {
                    const uint32_t bit = uint32_t(std::countr_zero(bits));
                    std::memcpy(p, &g_regs.values[baseRegister + (i << 8) + (bit << 2)], 16);
                    p += 16;
                }
        }
        std::memset(p, 0, size_t(end - p));
        return info;
    }

    // The constants set (set 1) for a draw. Dynamic mode: one set per (float range bucket pair)
    // per frame, bound with the buffers' offsets as dynamic offsets. Otherwise one set per draw.
    VkDescriptorSet ConstantsSet(const VkDescriptorBufferInfo* buffers, uint32_t* dynamicOffsets)
    {
        uint32_t key = 0;
        if (g_dynamicConstants)
        {
            for (uint32_t i = 0; i < Translator::kConstantBufferCount; ++i) dynamicOffsets[i] = uint32_t(buffers[i].offset);
            key = uint32_t(buffers[Translator::kConstantBufferFloatVertex].range) << 16 | uint32_t(buffers[Translator::kConstantBufferFloatPixel].range);
            if (auto it = g_constantSetCache.find(key); it != g_constantSetCache.end())
            {
                ++g_setsReused;
                return it->second;
            }
        }
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_layoutConstants;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(g_dev, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
        ++g_setsAllocated;
        // One write per binding: the bindings have different stage flags, so a single write
        // spanning them is invalid (VUID-VkWriteDescriptorSet-descriptorCount-10776).
        VkDescriptorBufferInfo infos[Translator::kConstantBufferCount];
        VkWriteDescriptorSet cw[Translator::kConstantBufferCount];
        for (uint32_t i = 0; i < Translator::kConstantBufferCount; ++i)
        {
            infos[i] = buffers[i];
            if (g_dynamicConstants) infos[i].offset = 0;
            cw[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            cw[i].dstSet = set;
            cw[i].dstBinding = i;
            cw[i].descriptorCount = 1;
            cw[i].descriptorType = g_dynamicConstants ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            cw[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(g_dev, Translator::kConstantBufferCount, cw, 0, nullptr);
        if (g_dynamicConstants) g_constantSetCache.emplace(key, set);
        return set;
    }

    std::vector<uint8_t> PackFloatConstants(const Shader& shader, uint32_t baseRegister)
    {
        const auto& map = shader.constant_register_map();
        std::vector<uint8_t> out(16 * std::max<uint32_t>(map.float_count, 1));
        uint8_t* p = out.data();
        for (uint32_t i = 0; i < 4; ++i)
            for (uint32_t bit = 0; bit < 64; ++bit)
                if (map.float_bitmap[i] & (1ull << bit))
                {
                    std::memcpy(p, &g_regs.values[baseRegister + (i << 8) + (bit << 2)], 16);
                    p += 16;
                }
        return out;
    }

    // The textures/samplers set for one stage. Identical sets within a frame are shared: the
    // views are still looked up every draw (uploads, layout transitions), only the descriptor
    // allocation and writes are skipped.
    VkDescriptorSet TextureSet(VkDescriptorSetLayout layout, const SpirvShader* shader)
    {
        if (!shader || layout == g_layoutEmpty)
        {
            layout = g_layoutEmpty;
            shader = nullptr;
        }
        static std::vector<VkDescriptorImageInfo> infos;
        infos.clear();
        size_t textureCount = 0;
        if (shader)
        {
            const auto& textures = shader->GetTextureBindingsAfterTranslation();
            const auto& samplers = shader->GetSamplerBindingsAfterTranslation();
            textureCount = textures.size();
            for (const auto& t : textures)
                infos.push_back({ VK_NULL_HANDLE, TextureView(t.fetch_constant, t.dimension), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
            for (const auto& s : samplers)
                infos.push_back({ GetSampler(s), VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED });
        }
        static std::vector<uint64_t> keyWords;
        keyWords.clear();
        keyWords.push_back(uint64_t(reinterpret_cast<uintptr_t>(layout)));
        for (const auto& info : infos)
            keyWords.push_back(info.imageView ? uint64_t(reinterpret_cast<uintptr_t>(info.imageView)) : uint64_t(reinterpret_cast<uintptr_t>(info.sampler)));
        const uint64_t key = XXH3_64bits(keyWords.data(), keyWords.size() * 8);
        if (auto it = g_textureSetCache.find(key); it != g_textureSetCache.end())
        {
            ++g_setsReused;
            return it->second;
        }

        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_descriptorPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(g_dev, &ai, &set) != VK_SUCCESS) return VK_NULL_HANDLE;
        ++g_setsAllocated;
        g_textureSetCache.emplace(key, set);
        if (infos.empty()) return set;
        std::vector<VkWriteDescriptorSet> writes(infos.size(), VkWriteDescriptorSet{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET });
        for (size_t i = 0; i < infos.size(); ++i)
        {
            writes[i].dstSet = set;
            writes[i].dstBinding = uint32_t(i);
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = i < textureCount ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[i].pImageInfo = &infos[i];
        }
        vkUpdateDescriptorSets(g_dev, uint32_t(writes.size()), writes.data(), 0, nullptr);
        return set;
    }
}
