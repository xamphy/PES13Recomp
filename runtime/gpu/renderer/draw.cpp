// Draws: index/vertex setup, rectangle lists, state -> pipeline, clears, the draw itself.
// Part of the renderer (see renderer_internal.h for the file map).
#include "renderer_internal.h"

namespace RendererImpl
{
    void DrawImpl(uint32_t indexBase, uint32_t indexSizeDword)
    {
        ScopedTimer t{ g_time.drawTotal };
        DrawImpl2(indexBase, indexSizeDword);
    }

    void DrawImpl2(uint32_t indexBase, uint32_t indexSizeDword)
    {
        const auto mode = g_regs.Get<reg::RB_MODECONTROL>().edram_mode;
        if (mode == xenos::EdramMode::kCopy)
        {
            ++g_stats.resolves;
            Resolve();
            return;
        }
        const auto init = g_regs.Get<reg::VGT_DRAW_INITIATOR>();
        VkPrimitiveTopology topology;
        bool convert;
        if (!HostTopology(init.prim_type, topology, convert))
        {
            WarnOnce("primitive type " + std::to_string(uint32_t(init.prim_type)) + " not drawn yet");
            ++g_stats.badPrim;
            if (g_capState == CaptureState::Active && g_current[0])
            {
                Shader& vsh = *g_current[0]->shader;
                xe::StringBuffer dis;
                if (!vsh.is_ucode_analyzed()) vsh.AnalyzeUcode(dis);
                char name[192];
                std::snprintf(name, sizeof(name), "%s/vs_%016llx.txt", g_capDir.c_str(), (unsigned long long)vsh.ucode_data_hash());
                if (!std::filesystem::exists(name))
                    if (FILE* f = std::fopen(name, "w")) { std::fputs(vsh.ucode_disassembly().c_str(), f); std::fclose(f); }
                CaptureStep(nullptr, "SKIPPED draw prim %u, %u idx, edram mode %u | vs %016llx memexport %u | rt %u",
                    uint32_t(init.prim_type), uint32_t(init.num_indices), uint32_t(g_regs.Get<reg::RB_MODECONTROL>().edram_mode),
                    (unsigned long long)vsh.ucode_data_hash(), uint32_t(vsh.memexport_eM_written()), g_regs.Get<reg::RB_COLOR_INFO>().color_base);
            }
            return;
        }
        ShaderEntry* vs = g_current[0];
        if (!vs) { ++g_stats.noVs; return; }
        xe::StringBuffer disasm;
        if (!vs->shader->is_ucode_analyzed()) vs->shader->AnalyzeUcode(disasm);

        const bool polygonal = draw_util::IsPrimitivePolygonal(g_regs);
        if (!draw_util::IsRasterizationPotentiallyDone(g_regs, polygonal)) { ++g_stats.noRaster; return; }
        ShaderEntry* ps = mode == xenos::EdramMode::kColorDepth ? g_current[1] : nullptr;
        if (ps)
        {
            if (!ps->shader->is_ucode_analyzed()) ps->shader->AnalyzeUcode(disasm);
            if (!draw_util::IsPixelShaderNeededWithRasterization(*ps->shader, g_regs)) ps = nullptr;
        }
        const uint32_t colorMask = ps ? draw_util::GetNormalizedColorMask(g_regs, ps->shader->writes_color_targets()) & 0xF : 0;
        const auto depthControl = draw_util::GetNormalizedDepthControl(g_regs);
        const bool depthStencil = depthControl.z_enable || depthControl.stencil_enable;
        if (!colorMask && !depthStencil) { ++g_stats.noColor; return; }

        // Shader modifications (as Xenia's VulkanPipelineCache).
        const auto programCntl = g_regs.Get<reg::SQ_PROGRAM_CNTL>();
        uint32_t paramGenPos = UINT32_MAX;
        const uint32_t interpolatorMask = ps ? vs->shader->writes_interpolators() &
            ps->shader->GetInterpolatorInputMask(programCntl, g_regs.Get<reg::SQ_CONTEXT_MISC>(), paramGenPos) : 0;
        Translator::Modification vmod(g_translator->GetDefaultVertexShaderModification(
            vs->shader->GetDynamicAddressableRegisterCount(programCntl.vs_num_reg), Shader::HostVertexShaderType::kVertex));
        vmod.vertex.interpolator_mask = interpolatorMask;
        vmod.vertex.output_point_parameters = 0;
        Translator::Modification pmod(g_translator->GetDefaultPixelShaderModification(
            ps ? ps->shader->GetDynamicAddressableRegisterCount(programCntl.ps_num_reg) : 0));
        pmod.pixel.interpolator_mask = interpolatorMask;
        pmod.pixel.interpolators_centroid = interpolatorMask &
            ~xenos::GetInterpolatorSamplingPattern(g_regs.Get<reg::RB_SURFACE_INFO>().msaa_samples,
                g_regs.Get<reg::SQ_CONTEXT_MISC>().sc_sample_cntl, g_regs.Get<reg::SQ_INTERPOLATOR_CNTL>().sampling_pattern);
        if (paramGenPos < xenos::kMaxInterpolators)
        {
            pmod.pixel.param_gen_enable = 1;
            pmod.pixel.param_gen_interpolator = paramGenPos;
        }
        // Plain texture fetches (patched translator, xenia_patched/README.md): bits 49..63 of the
        // pixel shader modification mark fetch constants 0..14 that are unsigned in every swizzled
        // component (no gamma) and have no exponent bias - their fetches skip the run-time
        // signedness handling. PES_NO_PLAIN_FETCH=1: off (A/B).

        if (ps && !g_noPlainFetch)
        {
            uint64_t plain = 0;
            for (const auto& b : ps->shader->texture_bindings())
                if (b.fetch_constant < 15)
                {
                    const auto f = g_regs.GetTextureFetch(b.fetch_constant);
                    if (texture_util::SwizzleSigns(f) == 0 && f.exp_adjust == 0) plain |= uint64_t(1) << b.fetch_constant;
                }
            pmod.value |= plain << 49;
        }
        const VkShaderModule vsm = GetModule(*vs, vmod.value);
        const VkShaderModule psm = ps ? GetModule(*ps, pmod.value) : VK_NULL_HANDLE;
        if (!vsm || (ps && !psm)) { ++g_stats.noModule; return; }

        // Render target 0.
        const auto colorInfo = g_regs.Get<reg::RB_COLOR_INFO>();
        const VkFormat rtFormat = ColorRtFormat(colorInfo.color_format);
        BeginFrame();
        const auto surfaceInfo = g_regs.Get<reg::RB_SURFACE_INFO>();
        const uint32_t pitch32 = xenos::GetSurfacePitchTiles(surfaceInfo.surface_pitch, surfaceInfo.msaa_samples, false);
        RenderTarget* rt = GetRenderTarget(colorInfo.color_base, rtFormat, pitch32 << (Is64bpp(rtFormat) ? 1 : 0), surfaceInfo.msaa_samples);
        if (!rt) return;
        // Optional post effects turned off in the launch menu (post_effects.cpp).
        if (SkipPostEffectDraw(ps ? ps->shader->ucode_data_hash() : 0, rt))
        {
            CaptureStep(nullptr, "POST-SKIPPED draw prim %u, %u idx: rt %u pitch %u msaa %u | ps %016llx",
                uint32_t(init.prim_type), uint32_t(init.num_indices), rt->base, rt->pitchTiles, uint32_t(rt->msaa),
                (unsigned long long)ps->shader->ucode_data_hash());
            return;
        }
        RenderTarget* depthRt = depthStencil ? GetRenderTarget(g_regs.Get<reg::RB_DEPTH_INFO>().depth_base, kDepthFormat, pitch32,
            surfaceInfo.msaa_samples) : nullptr;
        if (depthRt) depthRt->depthFloat = g_regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8;
        // Logical extent (the images may still be shorter: EnsureRows below, before BeginPass).
        VkExtent2D passExtent{ rt->fullWidth, rt->fullHeight };
        if (depthRt) passExtent = { std::min(passExtent.width, depthRt->fullWidth), std::min(passExtent.height, depthRt->fullHeight) };
        const uint32_t kRtWidth = passExtent.width, kRtHeight = passExtent.height; // limits below: this pass

        // Viewport and scissor.
        draw_util::ViewportInfo vi;
        draw_util::GetHostViewportInfo(g_regs, 1, 1, false, kRtWidth, kRtHeight, true, depthControl, false, false,
            ps && ps->shader->writes_depth(), vi);
        draw_util::Scissor scissor;
        draw_util::GetScissor(g_regs, scissor);
        if (!vi.xy_extent[0] || !vi.xy_extent[1] || !scissor.extent[0] || !scissor.extent[1]) { ++g_stats.noViewport; return; }

        // Indices.
        const uint32_t count = init.num_indices;
        if (!count) return;
        std::vector<uint32_t> guestIndices;
        const bool dma = init.source_select == xenos::SourceSelect::kDMA;
        const auto scMode = g_regs.Get<reg::PA_SU_SC_MODE_CNTL>();
        const bool restartEnabled = dma && scMode.multi_prim_ib_ena;
        const uint32_t resetIndex = g_regs[XE_GPU_REG_VGT_MULTI_PRIM_IB_RESET_INDX];
        if (dma)
        {
            const bool index32 = init.index_size == xenos::IndexFormat::kInt32;
            const auto endian = xenos::Endian(indexSizeDword >> 30);
            const uint32_t bytes = count * (index32 ? 4 : 2);
            if (indexBase + bytes > kSharedMemorySize) return;
            std::vector<uint8_t> raw(((bytes + 3) & ~3u));
            std::memcpy(raw.data(), g_xeMemory.TranslatePhysical(indexBase), bytes);
            SwapDwords(raw.data(), raw.size(), endian);
            guestIndices.resize(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t v;
                if (index32) std::memcpy(&v, raw.data() + i * 4, 4);
                else { uint16_t h; std::memcpy(&h, raw.data() + i * 2, 2); v = h; }
                if (restartEnabled && v == (index32 ? resetIndex : (resetIndex & 0xFFFF))) v = 0xFFFFFFFFu;
                guestIndices[i] = v;
            }
        }
        std::vector<uint32_t> hostIndices;
        struct FetchOverride { uint32_t fetchConstant, addressDwords, sizeDwords; };
        std::vector<FetchOverride> rectFetchOverrides;
        const bool rectList = init.prim_type == xenos::PrimitiveType::kRectangleList;
        VkRect2D rectBounds{};        // single rectangle: covered pixels (from the CPU-run vertex shader)
        float rectInterp[16][4] = {}; // single rectangle: interpolator outputs, if equal at all corners (rectInterpMask)
        uint32_t rectInterpMask = 0;
        float rectDepth = 0.0f; bool rectDepthConst = false; // single rectangle: window depth, equal at all corners
        bool rectBoundsValid = false;
        if (convert)
        {
            auto idx = [&](uint32_t i) { return dma ? guestIndices[i] : i; };
            if (init.prim_type == xenos::PrimitiveType::kTriangleFan)
            {
                uint32_t start = 0;
                for (uint32_t i = 0; i < count; ++i)
                {
                    if (idx(i) == 0xFFFFFFFFu) { start = i + 1; continue; }
                    if (i >= start + 2)
                        hostIndices.insert(hostIndices.end(), { idx(start), idx(i - 1), idx(i) });
                }
            }
            else if (init.prim_type == xenos::PrimitiveType::kQuadList)
                for (uint32_t q = 0; q + 3 < count; q += 4)
                    hostIndices.insert(hostIndices.end(), { idx(q), idx(q + 1), idx(q + 2), idx(q), idx(q + 2), idx(q + 3) });
            else // rectangle list: expand to 4 vertices per rectangle in the scratch region
            {
                const uint32_t rects = count / 3;
                const int32_t baseIndex = g_regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
                auto floatCount = [](xenos::VertexFormat f) -> uint32_t {
                    switch (f)
                    {
                    case xenos::VertexFormat::k_32_FLOAT: return 1;
                    case xenos::VertexFormat::k_32_32_FLOAT: return 2;
                    case xenos::VertexFormat::k_32_32_32_FLOAT: return 3;
                    case xenos::VertexFormat::k_32_32_32_32_FLOAT: return 4;
                    default: return 0;
                    }
                };
                // The 4th corner mirrors the right-angle corner across the diagonal (the longest
                // edge, as Xenia's geometry shader). Positions are only known after the vertex
                // shader: run it on the CPU (Xenia's ShaderInterpreter); if it can't be
                // interpreted, the first 2+-component float attribute stands in for them.
                std::vector<uint8_t> corner(rects, 0);
                bool cornersFound = false;
                const auto rectT0 = std::chrono::steady_clock::now();
                struct RectTimer { std::chrono::steady_clock::time_point t0; ~RectTimer() { g_time.rectCorners += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(); ++g_time.rectDraws; } } rectTimer{ rectT0 };
                if (ShaderInterpreter::CanInterpretShader(*vs->shader))
                {
                    struct PositionSink : ShaderInterpreter::ExportSink
                    {
                        float pos[4] = {};
                        bool has = false;
                        float interp[16][4] = {};
                        uint32_t interpMask = 0; // bit i: interpolator i fully written
                        void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override
                        {
                            if (uint32_t(reg) < 16)
                            {
                                for (int i = 0; i < 4; ++i) if (mask & (1u << i)) interp[uint32_t(reg)][i] = value[i];
                                if (mask == 0xF) interpMask |= 1u << uint32_t(reg);
                                return;
                            }
                            if (reg != ucode::ExportRegister::kVSPosition) return;
                            for (int i = 0; i < 4; ++i) if (mask & (1u << i)) pos[i] = value[i];
                            has = true;
                        }
                    } sink;
                    static ShaderInterpreter interpreter(g_regs, g_xeMemory);
                    interpreter.SetShader(*vs->shader);
                    interpreter.SetExportSink(&sink);
                    const auto vte = g_regs.Get<reg::PA_CL_VTE_CNTL>();
                    const float sx = vte.vport_x_scale_ena ? g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE) : 1.0f;
                    const float sy = vte.vport_y_scale_ena ? g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE) : 1.0f;
                    cornersFound = true;
                    for (uint32_t r = 0; r < rects && cornersFound; ++r)
                    {
                        float p[3][2], pz[3] = {}, pw[3] = {};
                        for (uint32_t k = 0; k < 3; ++k)
                        {
                            sink.has = false;
                            sink.interpMask = 0;
                            interpreter.temp_registers()[0] = float((uint32_t(int32_t(idx(r * 3 + k)) + baseIndex)) & 0xFFFFFF);
                            interpreter.Execute();
                            if (!sink.has) { cornersFound = false; break; }
                            if (rects == 1)
                            {
                                if (k == 0) { std::memcpy(rectInterp, sink.interp, sizeof(rectInterp)); rectInterpMask = sink.interpMask; }
                                else if (sink.interpMask != rectInterpMask || std::memcmp(rectInterp, sink.interp, sizeof(rectInterp)) != 0) rectInterpMask = 0;
                            }
                            float x = sink.pos[0], y = sink.pos[1];
                            if (!vte.vtx_xy_fmt && sink.pos[3] != 0.0f) { x /= sink.pos[3]; y /= sink.pos[3]; }
                            p[k][0] = x * sx;
                            p[k][1] = y * sy;
                            pz[k] = sink.pos[2];
                            pw[k] = sink.pos[3];
                        }
                        if (!cornersFound) break;
                        auto len2 = [&](int i, int j) { const float dx = p[i][0] - p[j][0], dy = p[i][1] - p[j][1]; return dx * dx + dy * dy; };
                        const float e12 = len2(1, 2), e20 = len2(2, 0), e01 = len2(0, 1);
                        corner[r] = e12 >= e20 && e12 >= e01 ? 0 : (e20 >= e01 ? 1 : 2);
                        // A single rectangle's covered pixels (window coordinates, pixel centers at
                        // .5 after the D3D -0.5 offset): lets clears skip copying old tile content.
                        if (rects == 1)
                        {
                            // Window depth if the same at all corners (clear rectangles: see knownDepthClear in the draw).
                            if (pz[0] == pz[1] && pz[1] == pz[2] && pw[0] == pw[1] && pw[1] == pw[2])
                            {
                                float zw = pz[0];
                                if (!vte.vtx_z_fmt && pw[0] != 0.0f) zw /= pw[0];
                                if (vte.vport_z_scale_ena) zw *= g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE);
                                if (vte.vport_z_offset_ena) zw += g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
                                rectDepth = zw;
                                rectDepthConst = true;
                            }
                            const float ox = vte.vport_x_offset_ena ? g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET) : 0.0f;
                            const float oy = vte.vport_y_offset_ena ? g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) : 0.0f;
                            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
                            for (uint32_t k = 0; k < 3; ++k)
                            {
                                x0 = std::min(x0, p[k][0] + ox); x1 = std::max(x1, p[k][0] + ox);
                                y0 = std::min(y0, p[k][1] + oy); y1 = std::max(y1, p[k][1] + oy);
                            }
                            int32_t wx = 0, wy = 0;
                            if (scMode.vtx_window_offset_enable)
                            {
                                const auto wo = g_regs.Get<reg::PA_SC_WINDOW_OFFSET>();
                                wx = wo.window_x_offset;
                                wy = wo.window_y_offset;
                            }
                            const int32_t ix0 = int32_t(std::floor(x0 + 0.5f)) + wx, iy0 = int32_t(std::floor(y0 + 0.5f)) + wy;
                            const int32_t ix1 = int32_t(std::floor(x1 + 0.5f)) + wx, iy1 = int32_t(std::floor(y1 + 0.5f)) + wy;
                            if (ix1 > ix0 && iy1 > iy0)
                            {
                                rectBounds = { { ix0, iy0 }, { uint32_t(ix1 - ix0), uint32_t(iy1 - iy0) } };
                                rectBoundsValid = true;
                            }
                        }
                        static const double logFrom = [] { const char* e = std::getenv("PES_LOG_TILES"); return e ? std::atof(e) : -1.0; }();
                        static const auto logStart = std::chrono::steady_clock::now();
                        static uint32_t rectLogs = 0;
                        if (r == 0 && logFrom >= 0 && rectLogs < 400 &&
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - logStart).count() >= logFrom)
                        {
                            ++rectLogs;
                            std::printf("[rect] f%llu rt %u: (%g,%g) (%g,%g) (%g,%g) z %g %g %g w %g -> corner %u, %u rects, vte %08X, zscale %g zoffset %g\n",
                                (unsigned long long)g_frame, g_regs.Get<reg::RB_COLOR_INFO>().color_base, p[0][0], p[0][1], p[1][0], p[1][1],
                                p[2][0], p[2][1], pz[0], pz[1], pz[2], pw[0], uint32_t(corner[r]), rects, g_regs[XE_GPU_REG_PA_CL_VTE_CNTL],
                                g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE), g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET));
                        }
                    }
                    interpreter.SetExportSink(nullptr);
                    static uint32_t logs = 0;
                    if (logs < 6 && cornersFound && rects)
                    {
                        ++logs;
                        std::printf("[render] rect list: %u rects, first corner %u (CPU vertex shader)\n", rects, uint32_t(corner[0]));
                    }
                }
                if (!cornersFound)
                {
                    std::fill(corner.begin(), corner.end(), 0);
                    WarnOnce("rectangle list: vertex shader not interpretable on the CPU, guessing corners from attributes");
                    decltype(&vs->shader->vertex_bindings()[0]) pb = nullptr;
                    uint32_t posOffset = 0;
                    for (const auto& b : vs->shader->vertex_bindings())
                    {
                        for (const auto& attr : b.attributes)
                            if (floatCount(attr.fetch_instr.attributes.data_format) >= 2)
                            {
                                pb = &b;
                                posOffset = attr.fetch_instr.attributes.offset;
                                break;
                            }
                        if (pb) break;
                    }
                    if (pb && pb->stride_words && posOffset + 2 <= pb->stride_words)
                    {
                        const auto fetch = g_regs.GetVertexFetch(pb->fetch_constant);
                        for (uint32_t r = 0; r < rects; ++r)
                        {
                            float p[3][2];
                            bool ok = true;
                            for (uint32_t k = 0; k < 3 && ok; ++k)
                            {
                                const uint32_t address = (fetch.address + uint32_t(int32_t(idx(r * 3 + k)) + baseIndex) * pb->stride_words + posOffset) * 4;
                                if (address + 8 > kSharedMemorySize) { ok = false; break; }
                                uint32_t w[2];
                                std::memcpy(w, g_xeMemory.TranslatePhysical(address), 8);
                                SwapDwords(reinterpret_cast<uint8_t*>(w), 8, fetch.endian);
                                std::memcpy(p[k], w, 8);
                            }
                            if (!ok) break;
                            auto len2 = [&](int i, int j) { const float dx = p[i][0] - p[j][0], dy = p[i][1] - p[j][1]; return dx * dx + dy * dy; };
                            const float e12 = len2(1, 2), e20 = len2(2, 0), e01 = len2(0, 1);
                            corner[r] = e12 >= e20 && e12 >= e01 ? 0 : (e20 >= e01 ? 1 : 2);
                        }
                    }
                }
                for (const auto& binding : vs->shader->vertex_bindings())
                {
                    const auto fetch = g_regs.GetVertexFetch(binding.fetch_constant);
                    const uint32_t stride = binding.stride_words;
                    const uint32_t bytes = rects * 4 * stride * 4;
                    if (!stride || !g_rectScratch || g_rectScratchUsed + bytes > kRectScratchSize) { rects == 0 ? void() : WarnOnce("rect scratch full"); return; }
                    std::vector<uint32_t> out(size_t(rects) * 4 * stride);
                    for (uint32_t r = 0; r < rects; ++r)
                    {
                        uint32_t v[3][64] = {};
                        for (uint32_t k = 0; k < 3; ++k)
                        {
                            const uint32_t address = (fetch.address + uint32_t(int32_t(idx(r * 3 + k)) + baseIndex) * stride) * 4;
                            if (address + stride * 4 > kSharedMemorySize || stride > 64) return;
                            std::memcpy(v[k], g_xeMemory.TranslatePhysical(address), stride * 4);
                            SwapDwords(reinterpret_cast<uint8_t*>(v[k]), stride * 4, fetch.endian);
                        }
                        // Corner k (right angle); its neighbours a, b span the diagonal.
                        const uint32_t k0 = corner[r], ka = (k0 + 1) % 3, kb = (k0 + 2) % 3;
                        uint32_t v3[64];
                        std::memcpy(v3, v[k0], stride * 4); // non-float data: as the corner
                        for (const auto& attr : binding.attributes)
                        {
                            const auto& a = attr.fetch_instr.attributes;
                            const uint32_t floats = floatCount(a.data_format);
                            for (uint32_t c = 0; c < floats && a.offset + c < stride; ++c)
                            {
                                float fk, fa, fb;
                                std::memcpy(&fk, &v[k0][a.offset + c], 4);
                                std::memcpy(&fa, &v[ka][a.offset + c], 4);
                                std::memcpy(&fb, &v[kb][a.offset + c], 4);
                                const float f3 = fa + fb - fk;
                                std::memcpy(&v3[a.offset + c], &f3, 4);
                            }
                        }
                        uint32_t* dst = out.data() + size_t(r) * 4 * stride;
                        std::memcpy(dst, v[0], stride * 4);
                        std::memcpy(dst + stride, v[1], stride * 4);
                        std::memcpy(dst + 2 * stride, v[2], stride * 4);
                        std::memcpy(dst + 3 * stride, v3, stride * 4);
                    }
                    SwapDwords(reinterpret_cast<uint8_t*>(out.data()), out.size() * 4, fetch.endian); // back to guest order
                    VkDeviceSize offset = 0;
                    uint8_t* staging = g_uploads.Allocate(bytes, 16, offset);
                    if (!staging) return;
                    std::memcpy(staging, out.data(), bytes);
                    const uint32_t scratch = g_rectScratch + g_rectScratchUsed;
                    g_pendingCopies.push_back({ offset, scratch, bytes });
                    g_rectScratchUsed += (bytes + 255) & ~255u;
                    rectFetchOverrides.push_back({ binding.fetch_constant, scratch / 4, bytes / 4 });
                }
                for (uint32_t r = 0; r < rects; ++r)
                {
                    const uint32_t ka = (corner[r] + 1) % 3, kb = (corner[r] + 2) % 3;
                    hostIndices.insert(hostIndices.end(), { r * 4, r * 4 + 1, r * 4 + 2, r * 4 + ka, r * 4 + kb, r * 4 + 3 });
                }
            }
        }
        else if (dma)
            hostIndices = std::move(guestIndices);
        // Primitive restart: always on for strips. Host indices are 32-bit and 0xFFFFFFFF only
        // appears where the guest asked for a restart (its reset index, converted above), so this
        // draws the same as "off" - and it is what Metal does anyway: Metal cannot disable restart,
        // and MoltenVK logged "VK_ERROR_FEATURE_NOT_PRESENT: Metal does not support disabling
        // primitive restart" for every strip pipeline created with it off (200+ lines a session,
        // one per pipeline compile). One pipeline per strip state instead of two.
        const bool restart = !convert &&
            (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);

        // Vertex data: mirror the fetched ranges of guest memory.
        ScopedTimer vertexTimer{ g_time.vertexCopy };
        for (const auto& binding : vs->shader->vertex_bindings())
        {
            if (rectList) break; // rectangle data was expanded into the scratch region
            const auto fetch = g_regs.GetVertexFetch(binding.fetch_constant);
            const uint32_t address = fetch.address << 2, size = fetch.size << 2;
            if (address >= kSharedMemorySize || !size) continue;
            const uint32_t end = std::min(address + size, kSharedMemorySize);
            // Copy the pages not yet uploaded this frame - or changed by the CPU since (the game
            // fills vertex buffers while the frame is being drawn: a later draw's vertices can land
            // in a page an earlier draw already uploaded) - in contiguous runs. All uploads run
            // before the frame's draws, so the newest version of a page wins.
            const uint32_t firstPage = address >> 12;
            const uint32_t lastPage = (end - 1) >> 12;
            // Where the stream's pages live in g_shared (vertex heap, or identity).
            uint32_t heapFirstPage = firstPage << 12;
            if (!g_vertexIdentity)
            {
                heapFirstPage = MapVertexRange(firstPage << 12, (lastPage + 1) << 12);
                if (heapFirstPage == ~0u) { WarnOnce("vertex heap full (reset at the next frame)"); return; }
                rectFetchOverrides.push_back({ binding.fetch_constant, (heapFirstPage + (address & 0xFFFu)) / 4, fetch.size });
            }
            const bool hashPages = g_vbHash;
            std::vector<uint8_t> need(lastPage - firstPage + 1);
            for (uint32_t p = firstPage; p <= lastPage; ++p)
            {
                if (hashPages)
                {
                    const bool copied = g_pagesCopied[p >> 6] & (1ull << (p & 63));
                    need[p - firstPage] = !copied ||
                        XXH3_64bits(g_xeMemory.TranslatePhysical(p << 12), 4096) != g_pageHash[p];
                    if (copied && need[p - firstPage]) ++g_pagesRecopied;
                }
                else
                {
                    const bool valid = g_pagesValid[p >> 6] & (1ull << (p & 63));
                    need[p - firstPage] = !valid || MemoryWatch::WrittenSince(p << 12, 4096, g_pageCopyEpoch[p]);
                    if (valid && need[p - firstPage]) ++g_pagesRecopied;
                }
            }
            uint32_t page = firstPage;
            while (page <= lastPage)
            {
                if (!need[page - firstPage]) { ++page; continue; }
                uint32_t runEnd = page;
                while (runEnd + 1 <= lastPage && need[runEnd + 1 - firstPage]) ++runEnd;
                const uint32_t runBytes = (runEnd - page + 1) << 12;
                VkDeviceSize offset = 0;
                uint8_t* dst = g_uploads.Allocate(runBytes, 16, offset);
                if (!dst) break;
                // Epoch first, then protect, then copy: a write after the epoch read is recorded
                // (newer epoch) and makes the page dirty again; none can slip in unrecorded.
                const uint32_t epoch = MemoryWatch::Epoch();
                if (!hashPages) MemoryWatch::Protect(page << 12, runBytes);
                std::memcpy(dst, g_xeMemory.TranslatePhysical(page << 12), runBytes);
                g_pendingCopies.push_back({ offset, VkDeviceSize(heapFirstPage) + (VkDeviceSize(page - firstPage) << 12), runBytes });
                for (uint32_t p = page; p <= runEnd; ++p)
                {
                    g_pagesCopied[p >> 6] |= 1ull << (p & 63);
                    if (hashPages) g_pageHash[p] = XXH3_64bits(g_xeMemory.TranslatePhysical(p << 12), 4096);
                    else { g_pagesValid[p >> 6] |= 1ull << (p & 63); g_pageCopyEpoch[p] = epoch; }
                }
                g_time.vertexBytes += runBytes;
                page = runEnd + 1;
            }
        }
        vertexTimer.~ScopedTimer();
        new (&vertexTimer) ScopedTimer{ g_time.textures };

        // Textures (recorded before the render pass starts).
        const SpirvShader* pss = ps ? ps->shader.get() : nullptr;
        const uint32_t usedTextures = vs->shader->GetUsedTextureMaskAfterTranslation() | (pss ? pss->GetUsedTextureMaskAfterTranslation() : 0);
        PipelineLayout* layout = GetPipelineLayout(uint32_t(vs->shader->GetTextureBindingsAfterTranslation().size()),
            uint32_t(vs->shader->GetSamplerBindingsAfterTranslation().size()),
            pss ? uint32_t(pss->GetTextureBindingsAfterTranslation().size()) : 0,
            pss ? uint32_t(pss->GetSamplerBindingsAfterTranslation().size()) : 0);
        const auto texSetStart = std::chrono::steady_clock::now();
        const VkDescriptorSet vertexTextures = TextureSet(layout->textures[0], vs->shader.get());
        const VkDescriptorSet pixelTextures = TextureSet(layout->textures[1], pss);
        const auto constStart = std::chrono::steady_clock::now();
        g_texSetUs += std::chrono::duration_cast<std::chrono::microseconds>(constStart - texSetStart).count();

        // Constants.
        Translator::SystemConstants sc;
        FillSystemConstants(sc, polygonal, vi, usedTextures);
        if (rectList) sc.vertex_base_index = 0; // expanded vertices start at index 0
        VkDescriptorBufferInfo buffers[Translator::kConstantBufferCount];
        buffers[Translator::kConstantBufferSystem] = UploadUniform(&sc, sizeof(sc));
        buffers[Translator::kConstantBufferFloatVertex] = UploadFloatConstants(vs->shader.get(), XE_GPU_REG_SHADER_CONSTANT_000_X);
        buffers[Translator::kConstantBufferFloatPixel] = UploadFloatConstants(pss, XE_GPU_REG_SHADER_CONSTANT_256_X);
        buffers[Translator::kConstantBufferBoolLoop] = UploadUniform(&g_regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 4 * (8 + 32));
        if (rectFetchOverrides.empty())
            buffers[Translator::kConstantBufferFetch] = UploadUniform(&g_regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], 4 * 6 * 32);
        else
        {
            uint32_t fetchConstants[6 * 32];
            std::memcpy(fetchConstants, &g_regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], sizeof(fetchConstants));
            for (const auto& o : rectFetchOverrides)
            {
                uint32_t* fc = &fetchConstants[o.fetchConstant * 2];
                fc[0] = (fc[0] & 3u) | (o.addressDwords << 2);           // type 0:1, address 2:31
                fc[1] = (fc[1] & 0xFC000003u) | ((o.sizeDwords & 0xFFFFFF) << 2); // endian 0:1, size 2:25
            }
            buffers[Translator::kConstantBufferFetch] = UploadUniform(fetchConstants, sizeof(fetchConstants));
        }
        uint32_t dynamicOffsets[Translator::kConstantBufferCount];
        const VkDescriptorSet constants = ConstantsSet(buffers, dynamicOffsets);
        g_constUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - constStart).count();
        if (!constants || !vertexTextures || !pixelTextures)
        {
            WarnOnce("descriptor pool exhausted");
            return;
        }

        // Index buffer upload.
        VkDeviceSize indexOffset = 0;
        if (!hostIndices.empty())
        {
            uint8_t* p = g_uploads.Allocate(hostIndices.size() * 4, 4, indexOffset);
            if (!p) return;
            std::memcpy(p, hostIndices.data(), hostIndices.size() * 4);
        }
        else if (convert)
            return;

        // Pipeline.
        PipelineDesc pd{};
        pd.vs = vsm;
        pd.ps = psm;
        pd.topology = topology;
        pd.primitiveRestart = restart;
        // Face culling from PA_SU_SC_MODE_CNTL, as Xenia's Vulkan backend (same viewport origin).
        // Without it, double-sided meshes (front and back copies at the same depth, LEQUAL) showed
        // back-facing triangles: dark wedges on the ceremony flags (2026-11-08). Rectangle lists
        // are expanded on the CPU and stay unculled. PES_NO_CULL=1: no culling (A/B).
        static const bool noCull = std::getenv("PES_NO_CULL") != nullptr;
        pd.cull = VK_CULL_MODE_NONE;
        if (!noCull && !rectList && draw_util::IsPrimitivePolygonal(g_regs))
            pd.cull = (scMode.cull_front ? VK_CULL_MODE_FRONT_BIT : 0) | (scMode.cull_back ? VK_CULL_MODE_BACK_BIT : 0);
        pd.frontFace = scMode.face ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
        pd.colorMask = colorMask;
        pd.blend = g_regs[XE_GPU_REG_RB_BLENDCONTROL0] & 0x1FFF1FFF;
        pd.format = rtFormat;
        pd.depthFormat = depthRt ? kDepthFormat : VK_FORMAT_UNDEFINED;
        pd.depthControl = depthRt ? depthControl.value : 0;
        static const uint32_t noDepthIdx = [] { const char* e = std::getenv("PES_DEBUG_NODEPTH_IDX"); return e ? uint32_t(std::atoi(e)) : 0u; }();
        if (noDepthIdx && count == noDepthIdx) pd.depthControl &= ~0x7u; // experiment: no depth/stencil test or write
        pd.depthClamp = depthRt && g_regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
        pd.layout = layout;
        static const bool debugForceState = std::getenv("PES_DEBUG_FORCESTATE") != nullptr;
        if (debugForceState)
        {
            pd.colorMask = 0xF;
            pd.blend = 0x00010001;
        }
        VkPipeline pipeline = GetPipeline(pd);
        if (!pipeline) { ++g_stats.noPipeline; return; }
        // What this render target was last drawn from (PrepareResolveForCpu tells name/number tiles,
        // drawn from small glyph sheets, from kit LOD atlases, drawn from a 512x256 kit).
        if (colorMask && pss && !pss->GetTextureBindingsAfterTranslation().empty())
        {
            const auto f0 = g_regs.GetTextureFetch(pss->GetTextureBindingsAfterTranslation()[0].fetch_constant);
            rt->lastSourceW = std::max<uint32_t>(rt->lastSourceW, f0.size_2d.width + 1);
            rt->lastSourceH = std::max<uint32_t>(rt->lastSourceH, f0.size_2d.height + 1);
        }

        static const bool debugMagenta = std::getenv("PES_DEBUG_MAGENTA") != nullptr;
        if (debugMagenta && init.prim_type == xenos::PrimitiveType::kTriangleStrip)
        {
            VkClearColorValue magenta{};
            magenta.float32[0] = 1.0f; magenta.float32[2] = 1.0f; magenta.float32[3] = 1.0f;
            ClearImage(rt->img, magenta);
        }
        VkRect2D rect{ { int32_t(scissor.offset[0]), int32_t(scissor.offset[1]) }, { scissor.extent[0], scissor.extent[1] } };
        rect.extent.width = std::min<uint32_t>(rect.extent.width, kRtWidth - std::min<uint32_t>(scissor.offset[0], kRtWidth));
        rect.extent.height = std::min<uint32_t>(rect.extent.height, kRtHeight - std::min<uint32_t>(scissor.offset[1], kRtHeight));
        if (debugForceState)
            rect = { { 0, 0 }, { kRtWidth, kRtHeight } };
        bool colorOverwrites = false, depthOverwrites = false; // this draw writes every pixel of rectBounds (no load needed)
        std::vector<TileFill> colorFills, depthFills; // cleared tiles claimed from other targets: cleared in the pass (AcquireTiles)
        // Bring in the current content of the EDRAM tiles this draw touches (before the pass):
        // scissor and viewport (the scissor alone is often the whole surface).
        {
            int32_t x0 = std::max<int32_t>(rect.offset.x, int32_t(vi.xy_offset[0]));
            int32_t y0 = std::max<int32_t>(rect.offset.y, int32_t(vi.xy_offset[1]));
            int32_t x1 = std::min<int32_t>(rect.offset.x + int32_t(rect.extent.width), int32_t(vi.xy_offset[0] + vi.xy_extent[0]));
            int32_t y1 = std::min<int32_t>(rect.offset.y + int32_t(rect.extent.height), int32_t(vi.xy_offset[1] + vi.xy_extent[1]));
            // Unclipped full-surface draws (D3D clears): the vertices' real extent, by running the
            // vertex shader on the CPU (Xenia's DrawExtentEstimator).
            {
                const auto t0 = std::chrono::steady_clock::now();
                y1 = std::min<int32_t>(y1, int32_t(g_extentEstimator.EstimateMaxY(true, *vs->shader)));
                g_time.extentEstimate += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
            }
            // A single rectangle (D3D clears) touches only its own bounds. Claiming the whole scissor
            // took tiles the rectangle never writes: D3D clears a 4x-MSAA depth surface with a rectangle
            // over its first tile column only, the claim moved the second column to the depth target
            // (color -> depth copies are not done), and the stale depth came back into the color target
            // (opaque block behind jersey numbers, 2026-11-08). PES_NO_RECT_CLAIM=1: old behaviour (A/B).
            static const bool noRectClaim = std::getenv("PES_NO_RECT_CLAIM") != nullptr;
            if (rectBoundsValid && !noRectClaim)
            {
                x0 = std::max<int32_t>(x0, rectBounds.offset.x);
                y0 = std::max<int32_t>(y0, rectBounds.offset.y);
                x1 = std::min<int32_t>(x1, rectBounds.offset.x + int32_t(rectBounds.extent.width));
                y1 = std::min<int32_t>(y1, rectBounds.offset.y + int32_t(rectBounds.extent.height));
            }
            // Color and depth bound together cannot overlap in EDRAM: clamp each to the distance to the next base.
            auto maxYFor = [&](const RenderTarget* t, const RenderTarget* other) {
                if (!other || other->base <= t->base) return y1;
                const uint32_t tiles = other->base - t->base;
                return std::min<int32_t>(y1, int32_t((tiles + t->pitchTiles - 1) / t->pitchTiles * t->tileH));
            };
            // Both attachments' images must cover the draw's rows (the framebuffer is the smaller of them).
            if (y1 > 0)
            {
                EnsureRows(rt, uint32_t(y1));
                if (depthRt) EnsureRows(depthRt, uint32_t(y1));
            }
            if (x1 > x0)
            {
                const int32_t cy1 = maxYFor(rt, depthRt), dy1 = depthRt ? maxYFor(depthRt, colorMask ? rt : nullptr) : 0;
                // Clears (single rectangles that overwrite everything they cover) don't need the old
                // content of the tiles inside the rectangle.
                const VkRect2D* colorNoCopy = nullptr;
                const VkRect2D* depthNoCopy = nullptr;
                static const bool noClearSkip = std::getenv("PES_NO_CLEAR_SKIP") != nullptr; // A/B: always copy
                if (rectBoundsValid && !noClearSkip)
                {
                    const auto colorCtl = g_regs.Get<reg::RB_COLORCONTROL>();
                    if (colorMask == 0xF && pd.blend == 0x00010001 && !colorCtl.alpha_test_enable && !colorCtl.alpha_to_mask_enable &&
                        !(ps && ps->shader->kills_pixels()))
                        colorNoCopy = &rectBounds;
                    const uint32_t front = g_regs[XE_GPU_REG_RB_STENCILREFMASK], back = g_regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
                    auto stencilOverwrites = [](xenos::CompareFunction f, xenos::StencilOp pass, uint32_t refMask) {
                        return f == xenos::CompareFunction::kAlways && pass == xenos::StencilOp::kReplace && ((refMask >> 16) & 0xFF) == 0xFF;
                    };
                    if (depthControl.z_enable && depthControl.z_write_enable && depthControl.zfunc == xenos::CompareFunction::kAlways &&
                        depthControl.stencil_enable && stencilOverwrites(depthControl.stencilfunc, depthControl.stencilzpass, front) &&
                        (!depthControl.backface_enable || stencilOverwrites(depthControl.stencilfunc_bf, depthControl.stencilzpass_bf, back)) &&
                        !(ps && ps->shader->kills_pixels()))
                        depthNoCopy = &rectBounds;
                }
                colorOverwrites = colorNoCopy != nullptr;
                depthOverwrites = depthNoCopy != nullptr;
                CaptureNote("      claim: color %s %d,%d-%d,%d (tile %ux%u)%s | depth %s %d,%d-%d,%d%s", colorMask ? "yes" : "no", x0, y0, x1, cy1,
                    rt->tileW, rt->tileH, colorNoCopy ? " noCopy" : "", depthRt ? "yes" : "no", x0, y0, x1, dy1, depthNoCopy ? " noCopy" : "");
                if (colorMask && cy1 > y0) AcquireTiles(rt, x0, y0, uint32_t(x1 - x0), uint32_t(cy1 - y0), colorNoCopy, &colorFills, true);
                if (depthRt && dy1 > y0) AcquireTiles(depthRt, x0, y0, uint32_t(x1 - x0), uint32_t(dy1 - y0), depthNoCopy, &depthFills, true);
            }
            static const double logFrom = [] { const char* e = std::getenv("PES_LOG_TILES"); return e ? std::atof(e) : -1.0; }();
            static const auto logStart = std::chrono::steady_clock::now();
            static uint32_t drawLogs = 0;
            if (logFrom >= 0 && drawLogs < 400 &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - logStart).count() >= logFrom)
            {
                ++drawLogs;
                std::printf("[draw] f%llu prim %u rt %u fmt %d pitch %u msaa %u mask %X | depth %d ctl %08X | area %d,%d-%d,%d | ps %s\n",
                    (unsigned long long)g_frame, uint32_t(init.prim_type), rt->base, int(rt->format), rt->pitchTiles, uint32_t(rt->msaa),
                    colorMask, depthRt ? int(depthRt->base) : -1, depthControl.value, x0, y0, x1, y1, ps ? "yes" : "no");
            }
        }
        CaptureProbeNow(colorMask ? rt : nullptr, nullptr, true); // texel this draw blends onto (probe)
        // The draw's area for the pass (BeginPass): scissor and viewport - rasterization is clipped to
        // the viewport - plus a margin for wide lines. The scissor is narrowed to it, so the draw
        // cannot reach outside the pass area.
        {
            constexpr int32_t kMargin = 8;
            const int32_t vx0 = int32_t(vi.xy_offset[0]) - kMargin, vy0 = int32_t(vi.xy_offset[1]) - kMargin;
            const int32_t vx1 = int32_t(vi.xy_offset[0] + vi.xy_extent[0]) + kMargin, vy1 = int32_t(vi.xy_offset[1] + vi.xy_extent[1]) + kMargin;
            const int32_t ax0 = std::max(rect.offset.x, vx0), ay0 = std::max(rect.offset.y, vy0);
            const int32_t ax1 = std::min(rect.offset.x + int32_t(rect.extent.width), vx1);
            const int32_t ay1 = std::min(rect.offset.y + int32_t(rect.extent.height), vy1);
            if (ax1 > ax0 && ay1 > ay0)
                rect = { { ax0, ay0 }, { uint32_t(ax1 - ax0), uint32_t(ay1 - ay0) } };
            else
                rect.extent = { 0, 0 };
        }
        // A clear (a single rectangle that writes every pixel it covers, see the tile claim above)
        // starting at the origin: its area is exactly the rectangle (inside scissor and viewport),
        // and a new pass of that size need not load the old content (BeginPass).
        bool discardColor = false, discardDepth = false;
        if (rectBoundsValid && (colorOverwrites || depthOverwrites))
        {
            const auto colorCtl = g_regs.Get<reg::RB_COLORCONTROL>();
            const bool noAlphaTest = !colorCtl.alpha_test_enable && !colorCtl.alpha_to_mask_enable;
            // Color is only fully written if depth / stencil tests cannot reject pixels.
            const bool testsPass = !depthRt ||
                ((!depthControl.z_enable || depthControl.zfunc == xenos::CompareFunction::kAlways) &&
                 (!depthControl.stencil_enable || (depthControl.stencilfunc == xenos::CompareFunction::kAlways &&
                     (!depthControl.backface_enable || depthControl.stencilfunc_bf == xenos::CompareFunction::kAlways))));
            const int32_t vx1 = int32_t(vi.xy_offset[0] + vi.xy_extent[0]), vy1 = int32_t(vi.xy_offset[1] + vi.xy_extent[1]);
            const int32_t ex1 = std::min({ rectBounds.offset.x + int32_t(rectBounds.extent.width), rect.offset.x + int32_t(rect.extent.width), vx1 });
            const int32_t ey1 = std::min({ rectBounds.offset.y + int32_t(rectBounds.extent.height), rect.offset.y + int32_t(rect.extent.height), vy1 });
            if (rectBounds.offset.x <= 0 && rectBounds.offset.y <= 0 && rect.offset.x == 0 && rect.offset.y == 0 &&
                int32_t(vi.xy_offset[0]) <= 0 && int32_t(vi.xy_offset[1]) <= 0 && ex1 > 0 && ey1 > 0 && noAlphaTest)
            {
                rect = { { 0, 0 }, { uint32_t(ex1), uint32_t(ey1) } };
                discardColor = colorOverwrites && testsPass;
                discardDepth = depthOverwrites && depthRt;
            }
        }
        // Tiles filled from clears (AcquireTiles) must be inside the pass area; the scissor stays `rect`.
        VkRect2D passNeed = rect;
        if (!colorFills.empty() || !depthFills.empty())
        {
            int32_t nx1 = rect.offset.x + int32_t(rect.extent.width), ny1 = rect.offset.y + int32_t(rect.extent.height);
            for (const auto* v : { &colorFills, &depthFills })
                for (const TileFill& t : *v)
                {
                    nx1 = std::max(nx1, t.rect.offset.x + int32_t(t.rect.extent.width));
                    ny1 = std::max(ny1, t.rect.offset.y + int32_t(t.rect.extent.height));
                }
            passNeed = { { 0, 0 }, { uint32_t(nx1), uint32_t(ny1) } };
            discardColor = discardDepth = false;
        }
        // A clear draw whose pixel shader outputs a constant (run twice on the CPU with different
        // inputs) on a plain 8888 target: the cleared tiles remember the value (g_tileClear).
        bool knownClear = false;
        VkClearValue clearValue{};
        VkRect2D clearArea{};
        if (colorOverwrites && rectBoundsValid && !g_noClearFill && ps && rt->format == VK_FORMAT_R8G8B8A8_UNORM &&
            ShaderInterpreter::CanInterpretShader(*ps->shader))
        {
            const auto ci = g_regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
            const bool testsPass = !depthRt ||
                ((!depthControl.z_enable || depthControl.zfunc == xenos::CompareFunction::kAlways) &&
                 (!depthControl.stencil_enable || (depthControl.stencilfunc == xenos::CompareFunction::kAlways &&
                     (!depthControl.backface_enable || depthControl.stencilfunc_bf == xenos::CompareFunction::kAlways))));
            ++g_clearDraws;
            if (ci.color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8 && ci.color_exp_bias == 0 && testsPass)
            {
                struct ColorSink : ShaderInterpreter::ExportSink
                {
                    float color[4] = {};
                    uint32_t mask = 0;
                    void Export(ucode::ExportRegister reg, const float* value, uint32_t m) override
                    {
                        if (reg != ucode::ExportRegister::kPSColor0) return;
                        for (int i = 0; i < 4; ++i) if (m & (1u << i)) color[i] = value[i];
                        mask |= m;
                    }
                };
                static ShaderInterpreter psInterpreter(g_regs, g_xeMemory);
                ColorSink a, b;
                psInterpreter.SetShader(*ps->shader);
                for (int run = 0; run < 2; ++run)
                {
                    float* t = psInterpreter.temp_registers();
                    for (int i = 0; i < int(xenos::kMaxShaderTempRegisters) * 4; ++i) t[i] = run ? 0.37f + float(i) * 0.011f : -1.9f - float(i) * 0.013f; // inputs (interpolators)
                    for (uint32_t r = 0; r < 16; ++r) // interpolators the vertex shader wrote, equal at all corners
                        if (rectInterpMask & (1u << r)) std::memcpy(t + r * 4, rectInterp[r], sizeof(rectInterp[r]));
                    psInterpreter.SetExportSink(run ? &b : &a);
                    psInterpreter.Execute();
                }
                psInterpreter.SetExportSink(nullptr);
                const bool finite = std::all_of(std::begin(a.color), std::end(a.color), [](float v) { return std::isfinite(v); });
                if (a.mask == 0xF && b.mask == 0xF && finite && std::equal(std::begin(a.color), std::end(a.color), std::begin(b.color)))
                {
                    const int32_t vx1 = int32_t(vi.xy_offset[0] + vi.xy_extent[0]), vy1 = int32_t(vi.xy_offset[1] + vi.xy_extent[1]);
                    const int32_t cx0 = std::max({ rectBounds.offset.x, rect.offset.x, int32_t(vi.xy_offset[0]) });
                    const int32_t cy0 = std::max({ rectBounds.offset.y, rect.offset.y, int32_t(vi.xy_offset[1]) });
                    const int32_t cx1 = std::min({ rectBounds.offset.x + int32_t(rectBounds.extent.width), rect.offset.x + int32_t(rect.extent.width), vx1 });
                    const int32_t cy1 = std::min({ rectBounds.offset.y + int32_t(rectBounds.extent.height), rect.offset.y + int32_t(rect.extent.height), vy1 });
                    if (cx1 > cx0 && cy1 > cy0 && cx0 >= 0 && cy0 >= 0)
                    {
                        for (int i = 0; i < 4; ++i) clearValue.color.float32[i] = std::clamp(a.color[i], 0.0f, 1.0f);
                        clearArea = { { cx0, cy0 }, { uint32_t(cx1 - cx0), uint32_t(cy1 - cy0) } };
                        knownClear = true;
                        ++g_clearDrawsKnown;
                    }
                }
            }
        }
        // A depth clear rectangle (writes depth and stencil everywhere it covers, see depthNoCopy):
        // its tiles remember the value only when it is exact in any format and conversion - window
        // depth exactly 0 or 1 (equal at all corners, viewport depth range 0..1, no shader depth
        // export) - and the stencil reference is the same for both faces.
        bool knownDepthClear = false;
        VkClearValue depthClearValue{};
        VkRect2D depthClearArea{};
        if (depthOverwrites && depthRt && rectBoundsValid && rectDepthConst && !g_noClearFill &&
            (rectDepth == 0.0f || rectDepth == 1.0f) && vi.z_min == 0.0f && vi.z_max == 1.0f && !(ps && ps->shader->writes_depth()) &&
            !g_regs.Get<reg::RB_COLORCONTROL>().alpha_test_enable && !g_regs.Get<reg::RB_COLORCONTROL>().alpha_to_mask_enable)
        {
            const uint32_t front = g_regs[XE_GPU_REG_RB_STENCILREFMASK], back = g_regs[XE_GPU_REG_RB_STENCILREFMASK_BF];
            if (!depthControl.backface_enable || (front & 0xFF) == (back & 0xFF))
            {
                const int32_t vx1 = int32_t(vi.xy_offset[0] + vi.xy_extent[0]), vy1 = int32_t(vi.xy_offset[1] + vi.xy_extent[1]);
                const int32_t cx0 = std::max({ rectBounds.offset.x, rect.offset.x, int32_t(vi.xy_offset[0]) });
                const int32_t cy0 = std::max({ rectBounds.offset.y, rect.offset.y, int32_t(vi.xy_offset[1]) });
                const int32_t cx1 = std::min({ rectBounds.offset.x + int32_t(rectBounds.extent.width), rect.offset.x + int32_t(rect.extent.width), vx1 });
                const int32_t cy1 = std::min({ rectBounds.offset.y + int32_t(rectBounds.extent.height), rect.offset.y + int32_t(rect.extent.height), vy1 });
                if (cx1 > cx0 && cy1 > cy0 && cx0 >= 0 && cy0 >= 0)
                {
                    depthClearValue.depthStencil = { rectDepth, front & 0xFF };
                    depthClearArea = { { cx0, cy0 }, { uint32_t(cx1 - cx0), uint32_t(cy1 - cy0) } };
                    knownDepthClear = true;
                    ++g_depthClearDrawsKnown;
                }
            }
        }
        // Lazy clear (see g_tilePending): a known clear of whole tiles is not drawn; its tiles keep
        // the value as pending and are filled by the next pass that uses them.
        if (!g_noLazyClear && !g_noClearFill && colorFills.empty() && depthFills.empty() &&
            !vs->shader->memexport_eM_written() && !(pss && pss->memexport_eM_written()))
        {
            auto sameRect = [](const VkRect2D& a, const VkRect2D& b) {
                return a.offset.x == b.offset.x && a.offset.y == b.offset.y && a.extent.width == b.extent.width && a.extent.height == b.extent.height;
            };
            const bool depthWritten = depthRt && (depthControl.z_write_enable || depthControl.stencil_enable);
            const bool colorOk = colorMask ? (knownClear && sameRect(clearArea, rect) && CanLazyClear(rt, rect)) : true;
            const bool depthOk = depthWritten ? (knownDepthClear && sameRect(depthClearArea, rect) && CanLazyClear(depthRt, rect)) : true;
            if (colorOk && depthOk && (colorMask || depthWritten))
            {
                if (colorMask) TryLazyClear(rt, rect, clearValue);
                if (depthWritten) TryLazyClear(depthRt, rect, depthClearValue);
                return;
            }
        }
        // A draw without depth/stencil on the color target of the open pass keeps that pass's depth
        // attachment (depth test, write and stencil off) instead of starting a color-only pass: the
        // game alternates depth and no-depth draws on its scene targets, and on the tile-based iPad
        // GPU every pass switch stores and reloads the targets. PES_NO_DEPTH_KEEP=1 /
        // PES_CONTROL "depthkeep off": a color-only pass (A/B).
        RenderTarget* passDepthRt = depthRt;
        if (!depthRt && !g_noDepthKeep && g_inPass && g_passColor == rt && g_passDepth)
        {
            const VkExtent2D fe = PassExtent(rt, g_passDepth);
            if (passNeed.offset.x + int32_t(passNeed.extent.width) <= int32_t(fe.width) &&
                passNeed.offset.y + int32_t(passNeed.extent.height) <= int32_t(fe.height))
            {
                PipelineDesc kd = pd;
                kd.depthFormat = kDepthFormat;
                kd.depthControl = 0; // no depth test / write, no stencil
                kd.depthClamp = false;
                if (const VkPipeline kp = GetPipeline(kd))
                {
                    pipeline = kp;
                    passDepthRt = g_passDepth;
                    ++g_depthKeptDraws;
                }
            }
        }
        BeginPass(rt, passDepthRt, &passNeed, discardColor, discardDepth && depthRt);
        if (!colorFills.empty() || !depthFills.empty())
        {
            // Thousands of tiles per frame: tiles next to each other in a row with the same value
            // become one rectangle (AcquireTiles lists them row by row), and each value is one call.
            for (const auto* v : { &colorFills, &depthFills })
            {
                std::vector<std::pair<VkClearValue, std::vector<VkClearRect>>> groups;
                for (const TileFill& t : *v)
                {
                    auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return !std::memcmp(&x.first, &t.value, sizeof(VkClearValue)); });
                    if (g == groups.end()) { groups.push_back({ t.value, {} }); g = groups.end() - 1; }
                    VkClearRect* last = g->second.empty() ? nullptr : &g->second.back();
                    if (last && last->rect.offset.y == t.rect.offset.y && last->rect.extent.height == t.rect.extent.height &&
                        last->rect.offset.x + int32_t(last->rect.extent.width) == t.rect.offset.x)
                        last->rect.extent.width += t.rect.extent.width;
                    else
                        g->second.push_back({ t.rect, 0, 1 });
                }
                for (const auto& [value, rects] : groups)
                {
                    VkClearAttachment att{};
                    att.aspectMask = v == &colorFills ? VK_IMAGE_ASPECT_COLOR_BIT : (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
                    att.colorAttachment = 0;
                    att.clearValue = value;
                    vkCmdClearAttachments(g_cmd, 1, &att, uint32_t(rects.size()), rects.data());
                }
            }
            if (!colorFills.empty()) ++rt->writeSerial;
            if (!depthFills.empty() && depthRt) ++depthRt->writeSerial;
        }
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        // Stencil reference/masks: RB_STENCILREFMASK(_BF) = ref 0:7, compare mask 8:15, write mask 16:23.
        {
            const uint32_t front = g_regs[XE_GPU_REG_RB_STENCILREFMASK];
            const uint32_t back = depthControl.backface_enable ? g_regs[XE_GPU_REG_RB_STENCILREFMASK_BF] : front;
            vkCmdSetStencilReference(g_cmd, VK_STENCIL_FACE_FRONT_BIT, front & 0xFF);
            vkCmdSetStencilReference(g_cmd, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
            vkCmdSetStencilCompareMask(g_cmd, VK_STENCIL_FACE_FRONT_BIT, (front >> 8) & 0xFF);
            vkCmdSetStencilCompareMask(g_cmd, VK_STENCIL_FACE_BACK_BIT, (back >> 8) & 0xFF);
            vkCmdSetStencilWriteMask(g_cmd, VK_STENCIL_FACE_FRONT_BIT, (front >> 16) & 0xFF);
            vkCmdSetStencilWriteMask(g_cmd, VK_STENCIL_FACE_BACK_BIT, (back >> 16) & 0xFF);
        }
        const VkDescriptorSet sets[Translator::kDescriptorSetCount] = { g_sharedSet, constants, vertexTextures, pixelTextures };
        vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout->layout, 0, Translator::kDescriptorSetCount, sets,
            g_dynamicConstants ? Translator::kConstantBufferCount : 0, g_dynamicConstants ? dynamicOffsets : nullptr);
        const VkViewport viewport{ float(vi.xy_offset[0]), float(vi.xy_offset[1]), float(vi.xy_extent[0]), float(vi.xy_extent[1]),
            vi.z_min, vi.z_max };
        vkCmdSetViewport(g_cmd, 0, 1, &viewport);
        vkCmdSetScissor(g_cmd, 0, 1, &rect);
        const float blendConstants[4] = { g_regs.Get<float>(XE_GPU_REG_RB_BLEND_RED), g_regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
            g_regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE), g_regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA) };
        vkCmdSetBlendConstants(g_cmd, blendConstants);
        MarkGpu(3);
        if (!hostIndices.empty())
        {
            vkCmdBindIndexBuffer(g_cmd, g_uploads.buf.buffer, indexOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(g_cmd, uint32_t(hostIndices.size()), 1, 0, 0, 0);
        }
        else
            vkCmdDraw(g_cmd, count, 1, 0, 0);
        g_markShader = ps ? ps->shader->ucode_data_hash() : 1; // 1: no pixel shader
        for (uint64_t& h : g_passPs) { if (h == g_markShader) break; if (!h) { h = g_markShader; break; } } // pass label
        MarkGpu(0);
        if (g_passLogNow && g_passLogDraws.size() < 600)
        {
            char d[96];
            std::snprintf(d, sizeof(d), " %s%04llx@%d,%d+%ux%u", knownClear || knownDepthClear ? "CLR:" : "",
                (unsigned long long)(g_markShader & 0xFFFF), rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
            g_passLogDraws += d;
        }
        if (knownClear) MarkTilesCleared(rt, clearArea, clearValue); // after AcquireTiles forgot the old values
        if (knownDepthClear) MarkTilesCleared(depthRt, depthClearArea, depthClearValue);
        g_markShader = 0;
        ++g_stats.issued;
        ++g_passDraws;
        if (g_passColor) ++g_passColor->writeSerial;
        if (g_passDepth && depthRt) ++g_passDepth->writeSerial; // a kept depth attachment is not written
        g_passX0 = std::min(g_passX0, rect.offset.x);
        g_passY0 = std::min(g_passY0, rect.offset.y);
        g_passX1 = std::max(g_passX1, rect.offset.x + int32_t(rect.extent.width));
        g_passY1 = std::max(g_passY1, rect.offset.y + int32_t(rect.extent.height));
        if (vs->shader->memexport_eM_written() || (pss && pss->memexport_eM_written()))
        {
            static uint32_t memexportLogs = 0;
            if (memexportLogs++ < 20)
                std::printf("[render] f%llu draw with MEMEXPORT (vs %u ps %u): prim %u, %u idx, rt %u\n", (unsigned long long)g_frame,
                    uint32_t(vs->shader->memexport_eM_written()), pss ? uint32_t(pss->memexport_eM_written()) : 0u, uint32_t(init.prim_type), count, rt->base);
        }
        // PES_CAPTURE_DUMP_IDX=<index count>: while capturing, write the full color target after
        // every draw with that many indices (<capture dir>/after_<step>.ppm). Flushes mid-frame.
        static const uint32_t dumpIdx = [] { const char* e = std::getenv("PES_CAPTURE_DUMP_IDX"); return e ? uint32_t(std::atoi(e)) : 0u; }();
        if (dumpIdx && count == dumpIdx && g_capState == CaptureState::Active && rt->format == VK_FORMAT_R8G8B8A8_UNORM && rt->base == 736)
            DumpImage(rt->img, std::min<uint32_t>(rt->img.width, 1280), std::min<uint32_t>(rt->img.height, 720),
                g_capDir + "/after_" + std::to_string(g_capStep) + ".ppm", false);
        std::string texList;
        if (g_capState == CaptureState::Active && !rectList)
        {
            // Vertex streams: address, size, first dwords, and whether a resolved surface overlaps them.
            for (const auto& b : vs->shader->vertex_bindings())
            {
                const auto vf = g_regs.GetVertexFetch(b.fetch_constant);
                const uint32_t va = vf.address << 2, vs_ = vf.size << 2;
                bool overSurface = false;
                for (auto& sf : g_surfaces)
                    if (va < sf->base + sf->size && va + vs_ > sf->base) overSurface = true;
                uint32_t w[4] = {};
                if (va + 16 <= kSharedMemorySize) std::memcpy(w, g_xeMemory.TranslatePhysical(va), 16);
                char one[160];
                std::snprintf(one, sizeof(one), " {vfc%u 0x%08X +%u stride %u: %08X %08X %08X %08X%s}", b.fetch_constant, va, vs_,
                    b.stride_words * 4, w[0], w[1], w[2], w[3], overSurface ? " OVER-SURFACE" : "");
                texList += one;
                // PES_CAPTURE_VB=<index count>: raw vertex stream of those draws -> vb_<step>_<addr>.bin
                static const uint32_t vbIdx = [] { const char* e = std::getenv("PES_CAPTURE_VB"); return e ? uint32_t(std::atoi(e)) : 0u; }();
                if (vbIdx && count == vbIdx && va + vs_ <= kSharedMemorySize)
                {
                    char vbName[192];
                    std::snprintf(vbName, sizeof(vbName), "%s/vb_%u_%08X.bin", g_capDir.c_str(), g_capStep, va);
                    if (FILE* vf = std::fopen(vbName, "wb")) { std::fwrite(g_xeMemory.TranslatePhysical(va), 1, vs_, vf); std::fclose(vf); }
                }
            }
            char vsName[48];
            std::snprintf(vsName, sizeof(vsName), " | vs %016llx", (unsigned long long)vs->shader->ucode_data_hash());
            texList += vsName;
            // PES_CAPTURE_CONSTS=<index count>: vertex shader float constants of those draws -> vsconst_<step>.txt
            static const uint32_t constIdx = [] { const char* e = std::getenv("PES_CAPTURE_CONSTS"); return e ? uint32_t(std::atoi(e)) : 0u; }();
            if (constIdx && count == constIdx)
                if (FILE* f = std::fopen((g_capDir + "/vsconst_" + std::to_string(g_capStep) + ".txt").c_str(), "w"))
                {
                    for (uint32_t c = 0; c < 256; ++c)
                    {
                        const float* v = reinterpret_cast<const float*>(&g_regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + c * 4]);
                        std::fprintf(f, "c%-3u %14.6g %14.6g %14.6g %14.6g\n", c, v[0], v[1], v[2], v[3]);
                    }
                    std::fclose(f);
                }
        }
        if (g_capState == CaptureState::Active)
        {
            // Vertex shader disassembly too (vertex fetch formats).
            char name[192];
            std::snprintf(name, sizeof(name), "%s/vs_%016llx.txt", g_capDir.c_str(), (unsigned long long)vs->shader->ucode_data_hash());
            if (!std::filesystem::exists(name))
                if (FILE* f = std::fopen(name, "w")) { std::fputs(vs->shader->ucode_disassembly().c_str(), f); std::fclose(f); }
        }
        if (g_capState == CaptureState::Active && pss)
        {
            char name[192];
            std::snprintf(name, sizeof(name), "%s/ps_%016llx.txt", g_capDir.c_str(), (unsigned long long)pss->ucode_data_hash());
            { char h[24]; std::snprintf(h, sizeof(h), "%016llx", (unsigned long long)pss->ucode_data_hash()); texList += std::string(" | ps ") + h; }
            if (!std::filesystem::exists(name))
                if (FILE* f = std::fopen(name, "w"))
                {
                    std::fputs(pss->ucode_disassembly().c_str(), f);
                    std::fclose(f);
                }
        }
        if (g_capState == CaptureState::Active && pss)
            for (const auto& t : pss->GetTextureBindingsAfterTranslation())
            {
                const auto f = g_regs.GetTextureFetch(t.fetch_constant);
                const uint32_t base = f.base_address << 12;
                bool inside = false;
                for (auto& sf : g_surfaces) if (base > sf->base && base < sf->base + sf->size) inside = true;
                const char* src = FindSurfaceForTexture(base, f) ? "surf" : inside ? "INSIDE-SURF" : (f.dimension != xenos::DataDimension::k2DOrStacked ? "dim" : "tex");
                char one[224];
                std::snprintf(one, sizeof(one), " [fc%u 0x%08X fmt %u %ux%u %s sw %03X | %s pitch %u stack %u mips %u-%u mipaddr 0x%08X%s dim %u endian %u]",
                    t.fetch_constant, base, uint32_t(f.format), uint32_t(f.size_2d.width) + 1, uint32_t(f.size_2d.height) + 1, src,
                    uint32_t(f.swizzle), f.tiled ? "tiled" : "linear", uint32_t(f.pitch) * 32, uint32_t(f.size_2d.stack_depth) + 1,
                    uint32_t(f.mip_min_level), uint32_t(f.mip_max_level), uint32_t(f.mip_address) << 12, f.packed_mips ? " packed" : "",
                    uint32_t(f.dimension), uint32_t(f.endianness));
                texList += one;
                // Raw guest bytes of small linear 32bpp textures (debug: inspect CPU-made sources such
                // as the jersey-number font sheets) -> <capture dir>/mem_<addr>_<w>x<h>.bin, once each.
                const uint32_t tw = uint32_t(f.size_2d.width) + 1, th = uint32_t(f.size_2d.height) + 1;
                if (!f.tiled && f.format == xenos::TextureFormat::k_8_8_8_8 && tw * th <= 512 * 512 &&
                    base + uint32_t(f.pitch) * 32 * th * 4 <= kSharedMemorySize)
                {
                    char mname[192];
                    std::snprintf(mname, sizeof(mname), "%s/mem_%08X_%ux%u.bin", g_capDir.c_str(), base, uint32_t(f.pitch) * 32, th);
                    if (!std::filesystem::exists(mname))
                        if (FILE* mf = std::fopen(mname, "wb"))
                        {
                            std::fwrite(g_xeMemory.TranslatePhysical(base), 1, size_t(f.pitch) * 32 * th * 4, mf);
                            std::fclose(mf);
                        }
                }
            }
        // PES_TRACE_RT="base,pitch": log every draw into that render target with its textures.
        static const std::array<int32_t, 2> traceRt = [] {
            std::array<int32_t, 2> v{ -1, -1 };
            if (const char* e = std::getenv("PES_TRACE_RT")) std::sscanf(e, "%d,%d", &v[0], &v[1]);
            return v;
        }();
        if (traceRt[0] >= 0 && int32_t(rt->base) == traceRt[0] && int32_t(rt->pitchTiles) == traceRt[1] && colorMask)
        {
            std::string tl;
            if (pss)
                for (const auto& t : pss->GetTextureBindingsAfterTranslation())
                {
                    const auto f = g_regs.GetTextureFetch(t.fetch_constant);
                    const uint32_t base = f.base_address << 12;
                    Surface* sf = FindSurfaceForTexture(base, f);
                    char one[128];
                    std::snprintf(one, sizeof(one), " [fc%u 0x%08X fmt %u %ux%u %s hash %016llx]", t.fetch_constant, base, uint32_t(f.format),
                        uint32_t(f.size_2d.width) + 1, uint32_t(f.size_2d.height) + 1, sf ? "surf" : "tex",
                        base + 4096 <= kSharedMemorySize ? (unsigned long long)XXH3_64bits(g_xeMemory.TranslatePhysical(base), 4096) : 0ull);
                    tl += one;
                }
            std::printf("[trace-rt] f%llu draw prim %u, %u idx: mask %X blend %08X alphatest %u | scissor %d,%d %ux%u |%s\n",
                (unsigned long long)g_frame, uint32_t(init.prim_type), count, colorMask, pd.blend,
                uint32_t(g_regs.Get<reg::RB_COLORCONTROL>().alpha_test_enable), rect.offset.x, rect.offset.y, rect.extent.width,
                rect.extent.height, tl.c_str());
        }
        g_capDepthRt = depthRt;
        CaptureStep(colorMask ? rt : nullptr, "draw prim %u, %u idx: rt %u fmt %d pitch %u msaa %u mask %X blend %08X | depth %d ctl %08X | scissor %d,%d %ux%u viewport %u,%u %ux%u | %s, %zu tex%s",
            uint32_t(init.prim_type), count, rt->base, int(rt->format), rt->pitchTiles, uint32_t(rt->msaa), colorMask, pd.blend,
            depthRt ? int(depthRt->base) : -1, depthControl.value, rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height,
            vi.xy_offset[0], vi.xy_offset[1], vi.xy_extent[0], vi.xy_extent[1], ps ? "ps" : "no ps",
            pss ? pss->GetTextureBindingsAfterTranslation().size() : size_t(0), texList.c_str());
        g_capDepthRt = nullptr;
        static uint32_t stripLogs = 0;
        if (init.prim_type == xenos::PrimitiveType::kTriangleStrip && stripLogs++ < 2)
        {
            std::printf("[render] strip: base index %d, min/max index %u/%u, fetch95 %08X %08X | alpha ref %g colorctl 0x%08X | z %.4f..%.4f, ndc scale %.4f %.4f %.4f offset %.4f %.4f %.4f, flags 0x%X, VTE 0x%08X, CLIP 0x%08X, vport regs %g %g %g %g %g %g, vertex bindings:",
                sc.vertex_base_index, g_regs[XE_GPU_REG_VGT_MIN_VTX_INDX], g_regs[XE_GPU_REG_VGT_MAX_VTX_INDX],
                g_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 190], g_regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + 191],
                g_regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF), g_regs[XE_GPU_REG_RB_COLORCONTROL],
                vi.z_min, vi.z_max, vi.ndc_scale[0], vi.ndc_scale[1], vi.ndc_scale[2], vi.ndc_offset[0], vi.ndc_offset[1], vi.ndc_offset[2],
                sc.flags, g_regs[XE_GPU_REG_PA_CL_VTE_CNTL], g_regs[XE_GPU_REG_PA_CL_CLIP_CNTL],
                g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XSCALE), g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_XOFFSET),
                g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE), g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET),
                g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZSCALE), g_regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_ZOFFSET));
            for (const auto& b : vs->shader->vertex_bindings())
            {
                const auto vf = g_regs.GetVertexFetch(b.fetch_constant);
                uint32_t first[3];
                std::memcpy(first, g_xeMemory.TranslatePhysical(vf.address << 2), 12);
                std::printf(" [fc %u addr 0x%X size %u first %08X %08X %08X]", b.fetch_constant, vf.address << 2, vf.size << 2,
                    first[0], first[1], first[2]);
            }
            std::printf("\n");
        }
        if (g_stats.issued <= 12)
            std::printf("[render] draw: prim %u, %u verts, rt base %u, viewport %u,%u %ux%u, scissor %u,%u %ux%u, ndc scale %.4f %.4f off %.3f %.3f, textures %zu\n",
                uint32_t(init.prim_type), count, colorInfo.color_base, vi.xy_offset[0], vi.xy_offset[1], vi.xy_extent[0], vi.xy_extent[1],
                scissor.offset[0], scissor.offset[1], scissor.extent[0], scissor.extent[1], vi.ndc_scale[0], vi.ndc_scale[1],
                vi.ndc_offset[0], vi.ndc_offset[1], pss ? pss->GetTextureBindingsAfterTranslation().size() : size_t(0));
    }
}
