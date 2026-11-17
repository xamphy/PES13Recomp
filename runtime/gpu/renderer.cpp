// Vulkan renderer for the command processor (see renderer.h). The per-draw logic follows
// Xenia's VulkanCommandProcessor::IssueDraw / UpdateSystemConstantValues / UpdateBindings
// (host render target path), using Xenia's shader translator and draw_util helpers.
#include "renderer/renderer_internal.h"

using namespace RendererImpl;


namespace Renderer
{
    void PrepareResolveForCpu(uint32_t guestAddr)
    {
        if (guestAddr < 0xA0000000u) return;
        const uint32_t physical = guestAddr & 0x1FFFFFFFu;
        MirroredResolve m;
        uint32_t base;
        {
            std::lock_guard lock(g_mirroredMutex);
            auto it = g_mirrored.upper_bound(physical);
            if (it == g_mirrored.begin()) return;
            --it;
            if (physical >= it->first + it->second.size) return;
            base = it->first;
            m = it->second;
            if (it->second.fromLargeSource) return; // a kit LOD atlas: read by the GPU later, keep its order
            g_mirrored.erase(it); // converted once; a new resolve registers again
        }
        if (base + m.size > kSharedMemorySize) return;
        // The GPU copy (surface) stays valid - only the CPU's view of the bytes changes - so the
        // write is not recorded (recording it made the GPU read these bytes instead: purple kits).
        MemoryWatch::UntrackedWrite(base, m.size, [](uint8_t* p, uint32_t size, void* user) {
            const MirroredResolve& mr = *static_cast<const MirroredResolve*>(user);
            for (uint32_t i = 0; i + 4 <= size; i += 4)
            {
                SwapDwords(p + i, 4, mr.endian); // undo the GPU-order transform: endian swap, then R/B
                if (mr.rbSwapped) std::swap(p[i], p[i + 2]);
            }
        }, &m);
        static uint32_t logs = 0;
        if (logs++ < 200) std::printf("[render] CPU reads resolve 0x%08X (%u bytes): stored in EDRAM channel order\n", base, m.size);
        ++g_cpuViewConversions;
    }

    void CompileTotals(uint64_t& pipelines, uint64_t& modules, uint64_t& us)
    {
        pipelines = g_pipelinesTotal, modules = g_modulesTotal, us = g_compileUsTotal;
    }

    void SetBlurEffects(bool on)
    {
        if (std::getenv("PES_POST_OFF")) return; // the test override wins (see Init)
        g_postOff = on ? 0 : ParsePostEffects("all");
    }

    bool Init()
    {
        if (const char* e = std::getenv("PES_POST_OFF")) g_postOff = ParsePostEffects(e);
        if (g_postOff)
        {
            std::string off;
            for (uint32_t i = 0; i < uint32_t(PostEffect::Count); ++i)
                if (g_postOff & (1u << i)) off += std::string(off.empty() ? "" : ",") + PostEffectName(PostEffect(i));
            std::printf("[render] post effects off: %s\n", off.c_str());
        }
        g_vk = Video::GetVulkanContext();
        MemoryWatch::Init();
        if (!g_vk) return false;
        g_dev = g_vk->device;
        g_bcDecode = !g_vk->enabledFeatures.textureCompressionBC || std::getenv("PES_FORCE_BC_DECODE") != nullptr;
        std::printf("[render] BC textures: %s\n", g_bcDecode ? "decoded on the CPU (RGBA8)" : "sampled directly");
        g_xeMemory.membase = Memory::Translate(0xA0000000u);
        // Vertex heap (see MapVertexRange): 64 MB by default (PES_VERTEX_HEAP_MB; ~38 MB used in a match), the rect scratch at 4 KB.
        g_sharedBytes = g_vertexIdentity ? kSharedMemorySize
            : VkDeviceSize([] { const char* e = std::getenv("PES_VERTEX_HEAP_MB"); return e ? std::max(16, std::atoi(e)) : 64; }()) << 20;
        g_rectScratch = g_vertexIdentity ? Kernel::AllocRuntimePhysical(kRectScratchSize) & 0x1FFFFFFF : 4096;
        std::memset(g_regs.values, 0, sizeof(g_regs.values));

        Translator::Features features(false);
        features.full_draw_index_uint32 = g_vk->enabledFeatures.fullDrawIndexUint32;
        features.image_view_format_swizzle = true;
        features.max_storage_buffer_range = g_vk->properties.limits.maxStorageBufferRange;
        g_translator = std::make_unique<Translator>(features, true, true, false);

        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = g_vk->queueFamily;
        vkCreateCommandPool(g_dev, &pci, nullptr, &g_cmdPool);
        VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = g_cmdPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        for (FrameSlot& s : g_slots)
        {
            vkAllocateCommandBuffers(g_dev, &cai, &s.cmd);
            vkAllocateCommandBuffers(g_dev, &cai, &s.uploadCmd);
            VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
            vkCreateFence(g_dev, &fci, nullptr, &s.fence);
        }
        {
            std::vector<uint8_t> initial;
            if (FILE* f = std::fopen(kPipelineCachePath, "rb"))
            {
                std::fseek(f, 0, SEEK_END);
                initial.resize(size_t(std::ftell(f)));
                std::fseek(f, 0, SEEK_SET);
                initial.resize(std::fread(initial.data(), 1, initial.size(), f));
                std::fclose(f);
            }
            VkPipelineCacheCreateInfo pc{ VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
            pc.initialDataSize = initial.size();
            pc.pInitialData = initial.empty() ? nullptr : initial.data();
            if (vkCreatePipelineCache(g_dev, &pc, nullptr, &g_pipelineCache) != VK_SUCCESS)
            {
                pc.initialDataSize = 0; // incompatible/corrupt data: start empty
                pc.pInitialData = nullptr;
                vkCreatePipelineCache(g_dev, &pc, nullptr, &g_pipelineCache);
            }
            std::printf("[render] pipeline cache: %zu bytes loaded\n", initial.size());
        }
        VkQueryPoolCreateInfo qpi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kMaxTimestamps;
        // Heap addresses must stay in the first shared-memory binding (the translator picks the binding
        // from the address in fixed 512 MB / N slices).
        if (!g_vertexIdentity)
            g_sharedBytes = std::min<VkDeviceSize>(g_sharedBytes, kSharedMemorySize >> g_translator->GetSharedMemoryStorageBufferCountLog2());
        bool buffersOk = CreateDeviceBuffer(g_shared, g_sharedBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        // Per-frame pools, one per frame slot. Measured peaks in a match (2026-11-09): uploads ~31 MB,
        // uniforms ~7 MB; they start at 48 / 8 MB (were 256 / 16) and grow after an overflow (BeginFrame).
        // PES_UPLOAD_MB=<n> sets the initial upload size.
        static const VkDeviceSize uploadBytes = VkDeviceSize([] { const char* e = std::getenv("PES_UPLOAD_MB"); return e ? std::max(16, std::atoi(e)) : 48; }()) << 20;
        g_uploads.target = uploadBytes;
        g_uniforms.target = 8u << 20;
        for (FrameSlot& s : g_slots)
        {
            vkCreateQueryPool(g_dev, &qpi, nullptr, &s.timestamps);
            buffersOk = buffersOk && CreateBuffer(s.uniforms, g_uniforms.target, kUniformPoolUsage) &&
                CreateBuffer(s.uploads, g_uploads.target, kUploadPoolUsage);
        }
        if (!buffersOk)
        {
            std::fprintf(stderr, "[render] could not allocate host-visible buffers - renderer disabled\n");
            return false;
        }

        // Descriptor set layouts (Xenia's layout for translated shaders).
        const uint32_t sharedBindings = 1u << g_translator->GetSharedMemoryStorageBufferCountLog2();
        const VkShaderStageFlags guestStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutBinding sharedBinding{ 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sharedBindings, guestStages, nullptr };
        VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lci.bindingCount = 1;
        lci.pBindings = &sharedBinding;
        vkCreateDescriptorSetLayout(g_dev, &lci, nullptr, &g_layoutShared);
        // Dynamic constant buffers need 5 dynamic uniform buffers per set (MoltenVK: check the limit).
        // PES_NO_DYNAMIC_CONSTANTS=1 forces the old one-set-per-draw path (A/B).
        g_dynamicConstants = g_vk->properties.limits.maxDescriptorSetUniformBuffersDynamic >= Translator::kConstantBufferCount &&
            !std::getenv("PES_NO_DYNAMIC_CONSTANTS");
        std::printf("[render] constant buffers: %s (device limit %u dynamic uniform buffers per set)\n",
            g_dynamicConstants ? "dynamic, one set per frame" : "one set per draw", g_vk->properties.limits.maxDescriptorSetUniformBuffersDynamic);
        VkDescriptorSetLayoutBinding constantBindings[Translator::kConstantBufferCount];
        for (uint32_t i = 0; i < Translator::kConstantBufferCount; ++i)
            constantBindings[i] = { i, g_dynamicConstants ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, guestStages, nullptr };
        constantBindings[Translator::kConstantBufferFloatVertex].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        constantBindings[Translator::kConstantBufferFloatPixel].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        lci.bindingCount = Translator::kConstantBufferCount;
        lci.pBindings = constantBindings;
        vkCreateDescriptorSetLayout(g_dev, &lci, nullptr, &g_layoutConstants);
        lci.bindingCount = 0;
        lci.pBindings = nullptr;
        vkCreateDescriptorSetLayout(g_dev, &lci, nullptr, &g_layoutEmpty);

        // Persistent set: shared memory, split into `sharedBindings` ranges.
        VkDescriptorPoolSize persistentSize{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sharedBindings };
        VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 1;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &persistentSize;
        vkCreateDescriptorPool(g_dev, &dpi, nullptr, &g_persistentPool);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_persistentPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_layoutShared;
        vkAllocateDescriptorSets(g_dev, &ai, &g_sharedSet);
        std::vector<VkDescriptorBufferInfo> sharedInfos(sharedBindings);
        for (uint32_t i = 0; i < sharedBindings; ++i)
            sharedInfos[i] = g_vertexIdentity
                ? VkDescriptorBufferInfo{ g_shared.buffer, VkDeviceSize(kSharedMemorySize / sharedBindings) * i, kSharedMemorySize / sharedBindings }
                : VkDescriptorBufferInfo{ g_shared.buffer, 0, g_sharedBytes }; // only binding 0 is ever addressed
        VkWriteDescriptorSet sw{ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        sw.dstSet = g_sharedSet;
        sw.descriptorCount = sharedBindings;
        sw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sw.pBufferInfo = sharedInfos.data();
        vkUpdateDescriptorSets(g_dev, 1, &sw, 0, nullptr);

        // Per-frame pool.
        const VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 8192 * Translator::kConstantBufferCount },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192 * Translator::kConstantBufferCount },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 32768 },
            { VK_DESCRIPTOR_TYPE_SAMPLER, 32768 },
        };
        dpi.maxSets = 8192 * 3;
        dpi.poolSizeCount = 4;
        dpi.pPoolSizes = sizes;
        for (FrameSlot& s : g_slots) vkCreateDescriptorPool(g_dev, &dpi, nullptr, &s.pool);

        BeginFrame();
        if (!CreateNullImage(g_null2D, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1) ||
            !CreateNullImage(g_null3D, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, 1) ||
            !CreateNullImage(g_nullCube, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, 6))
            return false;
        ClearImage(g_null2D, VkClearColorValue{});
        ClearImage(g_null3D, VkClearColorValue{});
        ClearImage(g_nullCube, VkClearColorValue{});
        EndFrame(true);

        g_active = true;
        std::printf("[render] Vulkan renderer ready (shared memory in %u binding(s))\n", sharedBindings);
        return true;
    }

    bool Active() { return g_active; }

    void DumpSurfacesAtFrame(uint64_t frame) { g_dumpSurfacesFrame = frame; }

    void WriteRegister(uint32_t index, uint32_t value)
    {
        if (index < RegisterFile::kRegisterCount)
            g_regs.values[index] = value;
    }

    void SetShader(int stage, const uint32_t* dwords, uint32_t count, uint64_t hash)
    {
        if (!g_active) return;
        const uint64_t key = hash ^ (stage ? 0x9E3779B97F4A7C15ull : 0);
        ShaderEntry& e = g_shaderCache[key];
        if (!e.shader)
            e.shader = std::make_unique<SpirvShader>(stage ? xenos::ShaderType::kPixel : xenos::ShaderType::kVertex, hash,
                dwords, count, std::endian::native);
        g_current[stage & 1] = &e;
    }

    void Draw(uint32_t indexBase, uint32_t indexSizeDword)
    {
        if (g_active)
            DrawImpl(indexBase, indexSizeDword);
    }

    bool Swap(uint32_t frontBufferPhysical, uint32_t width, uint32_t height, uint32_t swizzle)
    {
        if (!g_active) return false;
        // Slow swaps (> 100 ms) are logged with their parts (Gpu's "slow batch" names the swap).
        struct SwapTiming
        {
            std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
            uint64_t beginUs = 0, endFrameUs = 0, presentUs = 0;
            ~SwapTiming()
            {
                const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
                static uint32_t logs = 0;
                if (us > 100000 && logs++ < 20)
                    std::fprintf(stderr, "[render] slow swap: %llu ms (begin: wait for a free frame slot %llu, end of frame: record + submit %llu, present %llu, rest %llu)\n",
                        (unsigned long long)(us / 1000), (unsigned long long)(beginUs / 1000), (unsigned long long)(endFrameUs / 1000), (unsigned long long)(presentUs / 1000),
                        (unsigned long long)((us - beginUs - endFrameUs - presentUs) / 1000));
            }
        } swapTiming;
        {
            ScopedTimer beginTimer{ swapTiming.beginUs };
            BeginFrame();
            EndPass("swap");
        }
        Surface* s = FindSurfaceAt(frontBufferPhysical & 0x1FFFF000u);
        if (s) MarkSurfaceSampled(s); // the display reads it
        // The display reads the front buffer through the fetch constant's swizzle. D3D's ARGB
        // formats use X<->Z (red/blue exchanged): copy into a BGRA image, which reinterprets
        // the bytes with red and blue swapped, and present that.
        // Resolves with copy_dest_swap store red/blue exchanged in memory (kept unswapped in the image).
        const bool swapRedBlue = (((swizzle & 7) == 2 && ((swizzle >> 6) & 7) == 0)) != (s && s->rbSwapped);
        static bool loggedSwizzle = false;
        if (!loggedSwizzle)
        {
            loggedSwizzle = true;
            std::printf("[render] front buffer swizzle 0x%03X%s\n", swizzle, swapRedBlue ? " (red/blue swapped)" : "");
        }
        Image* present = s ? &s->img : nullptr;
        if (s && swapRedBlue && s->img.format == VK_FORMAT_R8G8B8A8_UNORM)
        {
            static Image swapped;
            if (swapped.width != s->img.width || swapped.height != s->img.height)
                CreateImage(swapped, VK_FORMAT_B8G8R8A8_UNORM, s->img.width, s->img.height,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            Transition(s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Transition(swapped, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy{};
            copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.extent = { s->img.width, s->img.height, 1 };
            vkCmdCopyImage(g_cmd, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapped.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            present = &swapped;
        }
        if (present) Transition(*present, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        {
            ScopedTimer endFrameTimer{ swapTiming.endFrameUs };
            EndFrame();
        }
        ++g_frame;
        g_passLogNow = g_passLogFrames > 0;
        if (g_passLogFrames) --g_passLogFrames;
        if (g_frame == g_dumpSurfacesFrame)
            DumpAllSurfaces();
        if (g_frame % 300 == 0)
            SavePipelineCache();
        if (g_frame % 120 == 0)
        {
            const Stats& t = g_stats;
            std::printf("[render] last 120 frames: %u draws issued, %u resolves | skipped: prim %u, no vs %u, no raster %u, no color %u, shader %u, viewport %u, pipeline %u | present %s\n",
                t.issued, t.resolves, t.badPrim, t.noVs, t.noRaster, t.noColor, t.noModule, t.noViewport, t.noPipeline, s ? "surface" : "fallback");
            const Timing& m = g_time;
            std::printf("[render] last 120 frames end of frame (ms): end recording %.1f, vkQueueSubmit %.1f, fence wait %.1f (GPU busy %.1f), present %.1f, EndFrame calls %llu | peak per-frame uploads %.1f MB, uniforms %.1f MB\n",
                m.endRecord / 1000.0, m.submit / 1000.0, m.fenceWait / 1000.0, m.gpuSpanUs / 1000.0, m.present / 1000.0, (unsigned long long)m.frames,
                g_uploadsPeak / 1048576.0, g_uniformsPeak / 1048576.0);
            std::printf("[render] last 120 frames CPU vertex shader (ms): extent estimates %.1f, rect corners + expansion %.1f (%llu rect draws), %llu vertex pages re-copied (changed mid-frame)\n",
                m.extentEstimate / 1e6, m.rectCorners / 1e6, (unsigned long long)m.rectDraws, (unsigned long long)g_pagesRecopied);
            {
                static uint64_t lastFaults = 0;
                const uint64_t f = MemoryWatch::Faults();
                std::printf("[render] last 120 frames: %llu write faults (vertex mode %s)\n", (unsigned long long)(f - lastFaults), g_vbHash ? "hash" : "track");
                lastFaults = f;
            }
            g_pagesRecopied = 0;
            std::printf("[render] last 120 frames (ms): texture sets %.0f (of which texture hashing %.0f for %.0f MB), constants %.0f\n",
                g_texSetUs / 1000.0, g_texHashUs / 1000.0, g_texHashBytes / 1048576.0, g_constUs / 1000.0);
            g_texSetUs = g_texHashUs = g_texHashBytes = g_constUs = 0;
            {
                std::lock_guard lock(g_wbStatsMutex);
                if (!g_wbStats.empty())
                {
                    std::vector<std::pair<uint32_t, uint64_t>> v;
                    for (auto& [k, n] : g_wbStats) v.push_back({ n, k });
                    std::sort(v.rbegin(), v.rend());
                    std::printf("[render] last 120 frames: %zu write-back targets, top:", v.size());
                    for (size_t i = 0; i < v.size() && i < 8; ++i)
                        std::printf(" 0x%08X %ux%u x%u;", uint32_t(v[i].second >> 32), uint32_t(v[i].second >> 16) & 0xFFFF, uint32_t(v[i].second & 0xFFFF), v[i].first);
                    std::printf("\n");
                    g_wbStats.clear();
                }
            }
            std::printf("[render] last 120 frames (ms): draws %.0f (vertex copy %.0f for %.0f MB, after-copy %.0f, %llu tex uploads; new shaders %llu in %.0f, new pipelines %llu in %.0f), submit+wait %.0f\n",
                m.drawTotal / 1000.0, m.vertexCopy / 1000.0, m.vertexBytes / 1048576.0, m.textures / 1000.0,
                (unsigned long long)m.textureUploads, (unsigned long long)m.newModules, m.modules / 1000.0,
                (unsigned long long)m.newPipelines, m.pipelines / 1000.0, m.submitWait / 1000.0);
            std::printf("[render] last 120 frames descriptor sets: %llu allocated, %llu reused\n",
                (unsigned long long)g_setsAllocated, (unsigned long long)g_setsReused);
            g_setsAllocated = g_setsReused = 0;
            std::printf("[render] last 120 frames (ms): resolves (CPU) %.0f | resolves %llu, copies skipped (never sampled) %llu = %.1f Mpx, skipped then sampled %llu (copies replayed %llu, lost %llu)\n",
                m.resolve / 1000.0, (unsigned long long)g_resolveSkip.resolves, (unsigned long long)g_resolveSkip.skipped,
                double(g_resolveSkip.skippedPixels) / 1e6, (unsigned long long)g_resolveSkip.staleSamples,
                (unsigned long long)g_resolveSkip.replayed, (unsigned long long)g_resolveSkip.lost);
            g_resolveSkip = {};
            {
                // Memory: process (what the OS counts) and Vulkan allocations by kind.
                constexpr double MB = 1024.0 * 1024.0;
                uint64_t rtBytes = 0, surfBytes = 0, cropBytes = 0, texBytes = 0, compBytes = 0, retired = 0, crops = 0;
                for (auto& [key, rt] : g_rts) rtBytes += rt->img.allocSize;
                for (auto& s : g_surfaces)
                {
                    surfBytes += s->img.allocSize;
                    retired += s->base == 0xFFFFFFFFu;
                    for (auto& [k, c] : s->crops) { cropBytes += c.img.allocSize; ++crops; }
                }
                for (auto& [key, t] : g_textures) texBytes += t->img.allocSize;
                for (auto& [key, c] : g_composed) compBytes += c->img.allocSize;
                const ProcessMemory::Info pm = ProcessMemory::Get();
                std::printf("[mem] process %.0f MB (peak %.0f), resident %.0f MB (peak %.0f) | vulkan: host buffers %.0f MB, "
                    "device buffers %.0f MB, images %.0f MB in %llu (%llu freed so far), upload pool overflows %llu, one-off staging %llu (%.0f MB total)\n",
                    pm.footprint / MB, pm.footprintPeak / MB, pm.resident / MB, pm.residentPeak / MB,
                    g_mem.hostBuffers / MB, g_mem.deviceBuffers / MB, g_mem.images / MB,
                    (unsigned long long)g_mem.imageCount, (unsigned long long)g_mem.imagesFreed, (unsigned long long)g_mem.poolExhausted,
                    (unsigned long long)g_stagingOneOff, g_stagingOneOffBytes / MB);
                std::printf("[mem] images: render targets %.0f MB (%zu), surfaces %.0f MB (%zu, %llu retired), crops %.0f MB (%llu), "
                    "textures %.0f MB (%zu), composed %.0f MB (%zu)\n",
                    rtBytes / MB, g_rts.size(), surfBytes / MB, g_surfaces.size(), (unsigned long long)retired,
                    cropBytes / MB, (unsigned long long)crops, texBytes / MB, g_textures.size(), compBytes / MB, g_composed.size());
                // Shared-memory mirror: which pages ever held vertex data (sizing a smaller window).
                {
                    uint64_t pages = 0;
                    int64_t lo = -1, hi = -1;
                    for (size_t w = 0; w < g_pagesValid.size(); ++w)
                        if (const uint64_t bits = g_pagesValid[w])
                        {
                            pages += uint64_t(__builtin_popcountll(bits));
                            if (lo < 0) lo = int64_t(w * 64 + __builtin_ctzll(bits));
                            hi = int64_t(w * 64 + 63 - __builtin_clzll(bits));
                        }
                    std::printf("[mem] vertex pages valid: %llu (%.1f MB), guest range 0x%08llX-0x%08llX (%.0f MB) | heap %.0f of %.0f MB used, %zu ranges, %llu merges, %llu resets\n",
                        (unsigned long long)pages, pages * 4096 / MB, (unsigned long long)(lo < 0 ? 0 : lo << 12),
                        (unsigned long long)(hi < 0 ? 0 : (hi + 1) << 12), lo < 0 ? 0.0 : (hi + 1 - lo) * 4096 / MB, g_vertexHeapTop / MB, g_sharedBytes / MB,
                        g_vertexRanges.size(), (unsigned long long)g_vertexRangeMerges, (unsigned long long)g_vertexHeapResets);
                }
                // Resolve surfaces by age (last resolve or sample) and whether guest memory has their content.
                {
                    uint64_t n[2][2] = {}, bytes[2][2] = {}; // [old][in memory]
                    for (auto& s : g_surfaces)
                    {
                        if (s->base == 0xFFFFFFFFu) continue;
                        const uint64_t last = std::max(s->lastResolveFrame, s->lastSampledFrame);
                        const int old = g_frame - last > 600, mem = s->writtenBackGen == s->generation;
                        ++n[old][mem];
                        bytes[old][mem] += s->img.allocSize;
                    }
                    std::printf("[mem] surfaces used in the last 600 frames: %llu (%.0f MB), older: %llu (%.0f MB) of which %llu (%.0f MB) also in guest memory; %llu evicted so far\n",
                        (unsigned long long)(n[0][0] + n[0][1]), (bytes[0][0] + bytes[0][1]) / MB, (unsigned long long)(n[1][0] + n[1][1]),
                        (bytes[1][0] + bytes[1][1]) / MB, (unsigned long long)n[1][1], bytes[1][1] / MB, (unsigned long long)g_surfacesEvicted);
                }
                // Every 10th report: each render target's size vs the rows actually used (sizing data).
                static uint32_t reports = 0;
                if (reports++ % 10 == 0)
                {
                    std::unordered_map<const RenderTarget*, uint32_t> owned;
                    for (const RenderTarget* o : g_tileOwner) if (o) ++owned[o];
                    std::printf("[mem] render targets (%zu, %llu evicted, %llu grows so far): base fmt pitch msaa | image WxH / logical rows MB grows | max row used | frames since use | owned tiles\n",
                        g_rts.size(), (unsigned long long)g_rtsEvicted, (unsigned long long)g_rtGrows);
                    for (auto& [key, rt] : g_rts)
                        std::printf("[mem]   %4u %3d %3u %u | %4ux%-4u /%4u %5.1f %u | %4u | %6llu | %4u\n", rt->base, int(rt->format), rt->pitchTiles,
                            uint32_t(rt->msaa), rt->img.width, rt->img.height, rt->fullHeight, rt->img.allocSize / MB, rt->grows, rt->maxRow,
                            (unsigned long long)(g_frame - rt->lastUsedFrame), owned.count(rt.get()) ? owned[rt.get()] : 0);
                }
            }
            std::printf("[render] last 120 frames textures: %llu composed from resolves (rebuilt), %llu composed fetches\n",
                (unsigned long long)g_composedBuilds, (unsigned long long)g_composedUses);
            g_composedBuilds = g_composedUses = 0;
            if (g_postOff)
            {
                std::printf("[post] last 120 frames, draws not issued:");
                for (uint32_t i = 0; i < uint32_t(PostEffect::Count); ++i)
                    std::printf(" %s %llu", PostEffectName(PostEffect(i)), (unsigned long long)g_postSkippedDraws[i]), g_postSkippedDraws[i] = 0;
                std::printf("\n");
            }
            std::printf("[render] last 120 frames GPU (ms): draws %.0f, resolves %.0f, uploads %.0f, EDRAM transfers %.0f, other %.0f | %u render pass begins, %u barriers, %llu EDRAM tiles moved (%llu not copied: overwritten, %llu filled from clears; clear draws %llu, constant %llu, depth %llu), %zu render targets\n",
                g_gpuMs[0], g_gpuMs[1], g_gpuMs[2], g_gpuMs[4], g_gpuMs[3], g_passBegins, g_barriers, (unsigned long long)g_tileTransfers, (unsigned long long)g_tilesSkipped, (unsigned long long)g_tilesFilled, (unsigned long long)g_clearDraws, (unsigned long long)g_clearDrawsKnown, (unsigned long long)g_depthClearDrawsKnown, g_rts.size());
            {
                const PassStats& s = g_passStats;
                std::printf("[passes] last 120 frames: %llu draws in %u passes (0 draws %u, 1 draw %u, 2-4 %u, 5+ %u) | framebuffer load+store %.1f Mpx, touched by draws %.1f Mpx, attachment loads skipped %.1f Mpx, no-depth draws kept in depth passes %llu, lazy clears %llu (flushed %llu, tiles pending now %u) | ended by:",
                    (unsigned long long)s.draws, s.byDraws[0] + s.byDraws[1] + s.byDraws[2] + s.byDraws[3], s.byDraws[0], s.byDraws[1], s.byDraws[2], s.byDraws[3],
                    s.framebufferPixels / 1e6, s.touchedPixels / 1e6, g_discardedLoadPixels / 1e6, (unsigned long long)g_depthKeptDraws, (unsigned long long)g_lazyClears, (unsigned long long)g_pendingFlushes, g_pendingTileCount);
                g_discardedLoadPixels = 0;
                g_depthKeptDraws = 0;
                g_lazyClears = g_pendingFlushes = 0;
                std::vector<std::pair<uint32_t, std::string>> reasons;
                for (const auto& [why, n] : s.endReasons) reasons.emplace_back(n, why);
                std::sort(reasons.rbegin(), reasons.rend());
                for (const auto& [n, why] : reasons) std::printf(" %s %u,", why.c_str(), n);
                std::printf("\n");
                g_passStats = {};
            }
            if (!g_passGpu.empty())
            {
                // Render passes by GPU time, labelled by target size / format / msaa and their first
                // pixel shaders (low 16 bits of the hash). Also on Apple (pass-edge timestamps).
                std::vector<std::pair<double, uint32_t>> top;
                double total = 0;
                for (const auto& [label, g] : g_passGpu) { top.push_back({ g.ms, label }); total += g.ms; }
                std::sort(top.rbegin(), top.rend());
                std::printf("[passgpu] last 120 frames, %.0f ms in passes; top:", total);
                for (size_t i = 0; i < top.size() && i < 10; ++i)
                {
                    const PassGpu& g = g_passGpu[top[i].second];
                    std::printf(" %s = %.1f ms (%u passes, %llu draws) |", g_passLabels[top[i].second].c_str(), g.ms, g.passes, (unsigned long long)g.draws);
                }
                std::printf("\n");
                g_passGpu.clear();
            }
            if (!g_shaderGpuMs.empty())
            {
                // Pixel shaders by GPU time (per-draw timestamps: not on Apple). 1 = no pixel shader.
                std::vector<std::pair<double, std::pair<uint64_t, uint32_t>>> top;
                for (const auto& [hash, e] : g_shaderGpuMs) top.push_back({ e.first, { hash, e.second } });
                std::sort(top.rbegin(), top.rend());
                std::printf("[shaders] last 120 frames, GPU ms by pixel shader:");
                for (size_t i = 0; i < top.size() && i < 8; ++i)
                    std::printf(" %016llx %.1f (%u draws),", (unsigned long long)top[i].second.first, top[i].first, top[i].second.second);
                std::printf("\n");
                g_shaderGpuMs.clear();
            }
            std::memset(g_gpuMs, 0, sizeof(g_gpuMs));
            g_passBegins = g_barriers = 0;
            g_tileTransfers = g_tilesSkipped = g_tilesFilled = g_clearDraws = g_clearDrawsKnown = g_depthClearDrawsKnown = 0;
            g_time = {};
            std::fflush(stdout);
            const uint32_t issued = g_stats.issued;
            g_stats = {};
            g_stats.issued = issued > 12 ? 13 : issued; // keep the first-draws log quiet
        }
        if (!present) return false;
        {
            ScopedTimer presentTimer{ g_time.present };
            ScopedTimer presentTotal{ swapTiming.presentUs };
            Video::PresentImage(present->image, std::min(width, present->width), std::min(height, present->height));
        }

        // PES_SHOTS="150,200": write the presented frame at those seconds after start to
        // userdata/shots/shot_<s>.ppm (test runs; no desktop capture needed).
        static std::vector<double> shots = [] {
            std::vector<double> v;
            if (const char* env = std::getenv("PES_SHOTS"))
                for (const char* p = env; *p;)
                {
                    v.push_back(std::atof(p));
                    while (*p && *p != ',') ++p;
                    if (*p == ',') ++p;
                }
            return v;
        }();
        static const auto start = std::chrono::steady_clock::now();
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        // PES_CONTROL=<file>: live test control. Lines appended to the file run once each, at the
        // next presented frame; each is acknowledged in <file>.ack ("ok <t> <line>" / "error ...").
        //   press <buttons> [seconds]   e.g. "press a", "press down+a 0.3" (default 0.2 s)
        //   shot <name>                 presented frame -> userdata/shots/<name>.ppm
        //   capture <name>              per-draw capture of the next frame -> userdata/capture/<name>/
        //                               (acknowledged "done capture <name>" when written)
        //   strict on|off               PES_STRICT_SURFACE_EXTENT switch
        {
            static const char* controlPath = std::getenv("PES_CONTROL");
            static uint64_t controlOffset = 0;
            static uint32_t controlFrame = 0;
            if (controlPath && (++controlFrame % 6) == 0) // ~10 polls per second
            {
                std::string text;
                if (FILE* f = std::fopen(controlPath, "rb"))
                {
                    std::fseek(f, 0, SEEK_END);
                    const long size = std::ftell(f);
                    if (size > long(controlOffset))
                    {
                        std::fseek(f, long(controlOffset), SEEK_SET);
                        text.resize(size_t(size - long(controlOffset)));
                        text.resize(std::fread(text.data(), 1, text.size(), f));
                    }
                    std::fclose(f);
                }
                const size_t lastNewline = text.rfind('\n');
                if (lastNewline != std::string::npos)
                {
                    controlOffset += lastNewline + 1;
                    FILE* ack = std::fopen((std::string(controlPath) + ".ack").c_str(), "ab");
                    size_t pos = 0;
                    while (pos <= lastNewline)
                    {
                        const size_t end = text.find('\n', pos);
                        std::string line = text.substr(pos, end - pos);
                        pos = end + 1;
                        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                        if (line.empty()) continue;
                        char cmd[32] = {}, arg[256] = {};
                        double seconds = 0.2;
                        const int n = std::sscanf(line.c_str(), "%31s %255s %lf", cmd, arg, &seconds);
                        bool ok = n >= 2 || (n == 1 && (!std::strcmp(cmd, "surfaces") || !std::strcmp(cmd, "threads")));
                        if (ok && !std::strcmp(cmd, "press"))
                            ok = Input::QueuePress(arg, seconds);
                        else if (ok && !std::strcmp(cmd, "shot"))
                        {
                            std::error_code ec;
                            std::filesystem::create_directories("userdata/shots", ec);
                            DumpImage(s->img, std::min(width, s->img.width), std::min(height, s->img.height),
                                std::string("userdata/shots/") + arg + ".ppm", swapRedBlue);
                        }
                        else if (ok && !std::strcmp(cmd, "capture"))
                            g_controlCapture.push_back(arg);
                        else if (ok && !std::strcmp(cmd, "watch-writes"))
                        {
                            // "watch-writes <physical hex> <size hex>": report the guest code writing there (debug)
                            const char* third = std::strchr(std::strchr(line.c_str(), ' ') + 1, ' ');
                            MemoryWatch::ReportNextWrite(uint32_t(std::strtoul(arg, nullptr, 16)), third ? uint32_t(std::strtoul(third + 1, nullptr, 16)) : 0x1000);
                        }
                        else if (ok && !std::strcmp(cmd, "watch-vwrites"))
                        {
                            // "watch-vwrites <guest hex> <size hex>": report guest code writing a heap range (debug)
                            const char* third = std::strchr(std::strchr(line.c_str(), ' ') + 1, ' ');
                            MemoryWatch::ReportVirtualWrites(uint32_t(std::strtoul(arg, nullptr, 16)), third ? uint32_t(std::strtoul(third + 1, nullptr, 16)) : 0x1000);
                        }
                        else if (ok && !std::strcmp(cmd, "capture-on-resolve"))
                        {
                            // "capture-on-resolve <name> <min address hex>": the first frame with a 128x128 resolve at or above it
                            const char* third = std::strchr(std::strchr(line.c_str(), ' ') + 1, ' ');
                            g_capArmMin = third ? uint32_t(std::strtoul(third + 1, nullptr, 16)) : 0;
                            g_capArmW = g_capArmH = 128;
                            if (const char* fourth = third ? std::strchr(third + 1, ' ') : nullptr) std::sscanf(fourth + 1, "%ux%u", &g_capArmW, &g_capArmH);
                            g_capArmed = true;
                            g_capArmHit = false;
                            g_controlCapture.push_back(arg);
                        }
                        else if (ok && !std::strcmp(cmd, "passes"))
                            g_fullPasses = !std::strcmp(arg, "full");
                        else if (ok && !std::strcmp(cmd, "passgpu"))
                            g_noPassGpu = !std::strcmp(arg, "off");
                        else if (ok && !std::strcmp(cmd, "lazyclear"))
                            g_noLazyClear = !std::strcmp(arg, "off");
                        else if (ok && !std::strcmp(cmd, "plainfetch"))
                            g_noPlainFetch = !std::strcmp(arg, "off");
                        else if (ok && !std::strcmp(cmd, "depthkeep"))
                            g_noDepthKeep = !std::strcmp(arg, "off");
                        else if (ok && !std::strcmp(cmd, "passlog"))
                            g_passLogFrames = uint32_t(std::max(1, std::atoi(arg)));
                        else if (n >= 2 && !std::strcmp(cmd, "post")) // "post glow,pause,soft|all|none": turn those off
                            g_postOff = ParsePostEffects(arg);
                        else if (ok && !std::strcmp(cmd, "resolveskip"))
                            g_noResolveSkip = !std::strcmp(arg, "off");
                        else if (ok && !std::strcmp(cmd, "discardloads"))
                            g_noDiscardLoads = !std::strcmp(arg, "off");
                        else if (ok && !std::strcmp(cmd, "clearfill"))
                            g_noClearFill = !std::strcmp(arg, "off");
                        else if (ok && !std::strcmp(cmd, "upscaler"))
                            Video::SetUpscaler(!std::strcmp(arg, "fsr") ? Video::Upscaler::Fsr1 : !std::strcmp(arg, "sgsr") ? Video::Upscaler::Sgsr1 : Video::Upscaler::Off);
                        else if (n >= 1 && !std::strcmp(cmd, "surfaces"))
                            ok = g_controlDumpSurfaces = true, g_controlDumpAllSurfaces = n >= 2 && !std::strcmp(arg, "all");
                        else if (n >= 1 && !std::strcmp(cmd, "threads"))
                            ok = true, Kernel::DumpGuestThreads();
                        else if (ok && !std::strcmp(cmd, "vb"))
                            g_vbHash = !std::strcmp(arg, "hash");
                        else if (ok && !std::strcmp(cmd, "sync"))
                            Kernel::SetSyncBroadcast(!std::strcmp(arg, "broadcast"));
                        else if (ok && !std::strcmp(cmd, "d3d"))
                            D3DWait::SetSpin(!std::strcmp(arg, "spin"));
                        else if (ok && !std::strcmp(cmd, "cp"))
                            Gpu::SetCpSpin(!std::strcmp(arg, "spin"));
                        else if (ok && !std::strcmp(cmd, "strict"))
                            g_strictExtent = !std::strcmp(arg, "on");
                        else
                            ok = false;
                        if (ack) std::fprintf(ack, "%s %.1f %s\n", ok ? "ok" : "error", t, line.c_str());
                        std::printf("[control] %s: %s\n", ok ? "ok" : "error", line.c_str());
                    }
                    if (ack) std::fclose(ack);
                }
            }
        }

        static const double strictExtentAt = [] { const char* e = std::getenv("PES_STRICT_SURFACE_EXTENT"); return e ? std::atof(e) : -1.0; }();
        if (!g_strictExtent && strictExtentAt >= 0 && t >= strictExtentAt)
        {
            g_strictExtent = true;
            std::printf("[render] strict surface extent ON at %.1f s (frame %llu)\n", t, (unsigned long long)g_frame);
        }
        for (double& at : shots)
            if (at >= 0 && t >= at)
            {
                std::error_code ec;
                std::filesystem::create_directories("userdata/shots", ec);
                char name[64];
                std::snprintf(name, sizeof(name), "userdata/shots/shot_%03d.ppm", int(at));
                DumpImage(s->img, std::min(width, s->img.width), std::min(height, s->img.height), name, swapRedBlue);
                at = -1;
            }

        static const double abAt = [] { const char* e = std::getenv("PES_AB_AT"); return e ? std::atof(e) : -1.0; }();
        if (g_abPhase == 1 || g_abPhase == 2)
        {
            std::error_code ec;
            std::filesystem::create_directories("userdata/shots", ec);
            DumpImage(s->img, std::min(width, s->img.width), std::min(height, s->img.height), g_abPhase == 1 ? "userdata/shots/ab_A.ppm" : "userdata/shots/ab_B.ppm", swapRedBlue);
            std::printf("[render] A/B frame %c written (frame %llu)\n", g_abPhase == 1 ? 'A' : 'B', (unsigned long long)g_frame);
            ++g_abPhase;
        }
        else if (g_abPhase == 0 && abAt >= 0 && t >= abAt)
            g_abPhase = 1;

        // PES_DUMP_SMALL_SURFACES=<seconds>: write every small (<= 256x256) RGBA8 surface to
        // userdata/surfaces/<base>.ppm once (kits, names, numbers).
        static const double dumpSmallAt = [] { const char* e = std::getenv("PES_DUMP_SMALL_SURFACES"); return e ? std::atof(e) : -1.0; }();
        static bool dumpedSmall = false;
        if ((!dumpedSmall && dumpSmallAt >= 0 && t >= dumpSmallAt) || g_controlDumpSurfaces)
        {
            dumpedSmall = true;
            g_controlDumpSurfaces = false;
            std::error_code ec;
            std::filesystem::create_directories("userdata/surfaces", ec);
            for (auto& [key, tex] : g_textures) // small RGBA8 textures read from guest memory, used recently
                if (tex && tex->img.format == VK_FORMAT_R8G8B8A8_UNORM && key[2] <= 1024 && key[3] <= 1024 && tex->uploadedFrame + 4 >= g_frame)
                {
                    char name[96];
                    std::snprintf(name, sizeof(name), "userdata/surfaces/tex_%08X_%ux%u_%s.ppm", key[0], key[2], key[3], key[4] ? "tiled" : "linear");
                    DumpImage(tex->img, key[2], key[3], name, false);
                }
            for (auto& [key, ct] : g_composed) // composed textures too (base, width, height, pitch, tiled)
                if (ct && ct->img.format == VK_FORMAT_R8G8B8A8_UNORM)
                {
                    char name[96];
                    std::snprintf(name, sizeof(name), "userdata/surfaces/composed_%08X_%ux%u_%s.ppm", key[0], key[1], key[2], key[4] ? "tiled" : "linear");
                    DumpImage(ct->img, key[1], key[2], name, ct->rbSwapped);
                }
            for (auto& sf : g_surfaces)
                if (sf->base != 0xFFFFFFFFu && ((sf->pitch <= 256 && sf->height <= 256) || g_controlDumpAllSurfaces) && sf->img.format == VK_FORMAT_R8G8B8A8_UNORM)
                {
                    char name[96];
                    std::snprintf(name, sizeof(name), "userdata/surfaces/%08X_%ux%u.ppm", sf->base, sf->pitch, sf->height);
                    DumpImage(sf->img, sf->pitch, sf->height, name, sf->rbSwapped);
                }
            // PES_DUMP_TEXTURE=0x...: the cached (uploaded) textures at that address, plus how much
            // of the guest bytes behind them is non-zero.
            if (const char* e = std::getenv("PES_DUMP_TEXTURE"))
            {
                const uint32_t want = uint32_t(std::strtoul(e, nullptr, 0));
                for (auto& [key, tex] : g_textures)
                    if (key[0] == want && tex && tex->img.format == VK_FORMAT_R8G8B8A8_UNORM)
                    {
                        const uint32_t w = key[2], h = key[3];
                        uint32_t nonZero = 0;
                        const uint8_t* g = g_xeMemory.TranslatePhysical(want);
                        for (uint32_t i = 0; i < w * h * 4 && want + i < kSharedMemorySize; ++i) nonZero += g[i] != 0;
                        std::printf("[render] texture 0x%08X fmt %u %ux%u tiled %u pitch %u: %u of %u guest bytes non-zero\n",
                            want, key[1], w, h, key[4], key[5], nonZero, w * h * 4);
#ifdef _WIN32
                        // Virtual addresses that MmGetPhysicalAddress (bits 29-31 stripped) maps here.
                        for (uint32_t virt : { want | 0x20000000u, want | 0x40000000u, want | 0x60000000u, want | 0x80000000u })
                        {
                            uint32_t nz = 0;
                            const uint8_t* g3 = Memory::Translate(virt);
                            MEMORY_BASIC_INFORMATION mbi{};
                            if (!VirtualQuery(g3, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
                            {
                                std::printf("[render]   virtual 0x%08X: not committed\n", virt);
                                continue;
                            }
                            for (uint32_t i = 0; i < w * h * 4 && i < mbi.RegionSize - (g3 - static_cast<const uint8_t*>(mbi.BaseAddress)); i += 64) nz += g3[i] != 0;
                            std::printf("[render]   virtual 0x%08X: %u of %u sampled bytes non-zero\n", virt, nz, w * h * 4 / 64);
                        }
#endif
                        for (int32_t shift : { -0x1000, 0x1000 })
                        {
                            uint32_t nz = 0;
                            const uint8_t* g2 = g_xeMemory.TranslatePhysical(want + shift);
                            for (uint32_t i = 0; i < w * h * 4; ++i) nz += g2[i] != 0;
                            std::printf("[render]   at %+d: %u non-zero\n", shift, nz);
                        }
                        char name[96];
                        std::snprintf(name, sizeof(name), "userdata/surfaces/tex_%08X_%ux%u.ppm", want, w, h);
                        DumpImage(tex->img, w, h, name, false);
                    }
            }
        }

        // Per-draw capture (PES_CAPTURE=<seconds>): a frame runs from one Swap to the next.
        // PES_CAPTURE="205,210,...": one capture per time, each into userdata/capture/<seconds>/.
        static const std::vector<double> captureTimes = [] {
            std::vector<double> v;
            if (const char* e = std::getenv("PES_CAPTURE"))
                for (const char* p = e; *p;)
                {
                    v.push_back(std::atof(p));
                    while (*p && *p != ',') ++p;
                    if (*p == ',') ++p;
                }
            return v;
        }();
        static size_t nextCapture = 0;
        // Armed capture (PES_CONTROL "capture-on-resolve"): a frame without the awaited resolve is
        // thrown away and the next frame is recorded instead.
        if (g_capState == CaptureState::Active && g_capArmed && !g_capArmHit)
        {
            std::fclose(g_capLog);
            g_capLog = nullptr;
            g_capState = CaptureState::Off;
            g_controlCapture.push_front(g_controlCaptureAck); // record the next frame into the same directory
            g_controlCaptureAck.clear();
        }
        else if (g_capState == CaptureState::Active)
        {
            if (g_capArmed) { g_capArmed = false; std::printf("[capture] armed capture hit (resolve 0x%08X)\n", g_capArmHitAddr); }
            std::fprintf(g_capLog, "%4u SWAP front buffer 0x%08X %ux%u (surface 0x%08X)\n", g_capStep, frontBufferPhysical, width, height, s->base);
            std::fclose(g_capLog);
            g_capLog = nullptr;
            const uint32_t rows = std::max<uint32_t>((g_capSlots + kThumbCols - 1) / kThumbCols, 1);
            DumpImage(g_capAtlas, kThumbCols * kThumbW, rows * kThumbH, g_capDir + "/atlas.ppm");
            DumpImage(s->img, std::min(width, s->img.width), std::min(height, s->img.height), g_capDir + "/final.ppm", swapRedBlue);
            // DumpImage waited for the GPU: the probe texels are in the buffer.
            if (!g_capProbes.empty())
                if (FILE* f = std::fopen((g_capDir + "/probes.txt").c_str(), "w"))
                {
                    for (const auto& p : g_capProbes)
                    {
                        std::fprintf(f, "%4u%s", p.step, p.pre ? " pre " : "     ");
                        if (p.color >= 0)
                        {
                            const uint8_t* c = g_capProbeBuf.mapped + p.color;
                            if (p.colorFormat == VK_FORMAT_R8G8B8A8_UNORM)
                                std::fprintf(f, "  color %3u %3u %3u %3u", c[0], c[1], c[2], c[3]);
                            else
                                std::fprintf(f, "  color(fmt %d) %02X%02X%02X%02X %02X%02X%02X%02X", int(p.colorFormat), c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7]);
                        }
                        if (p.depth >= 0)
                        {
                            float z;
                            std::memcpy(&z, g_capProbeBuf.mapped + p.depth, 4);
                            std::fprintf(f, "  depth %.7f", z);
                        }
                        if (p.stencil >= 0) std::fprintf(f, " stencil %u", g_capProbeBuf.mapped[p.stencil]);
                        std::fprintf(f, "\n");
                    }
                    std::fclose(f);
                }
            g_capState = (nextCapture < captureTimes.size() || std::getenv("PES_CONTROL")) ? CaptureState::Off : CaptureState::Done;
            if (!g_controlCaptureAck.empty())
            {
                if (FILE* ack = std::fopen((std::string(std::getenv("PES_CONTROL")) + ".ack").c_str(), "ab"))
                {
                    std::fprintf(ack, "done capture %s\n", g_controlCaptureAck.c_str());
                    std::fclose(ack);
                }
                g_controlCaptureAck.clear();
            }
            std::printf("[capture] frame %llu: %u steps, %u thumbnails -> %s\n", (unsigned long long)g_frame, g_capStep, g_capSlots, g_capDir.c_str());
        }
        if (g_capState == CaptureState::Off && (!g_controlCapture.empty() || (nextCapture < captureTimes.size() && t >= captureTimes[nextCapture])))
        {
            if (!g_controlCapture.empty())
            {
                g_capDir = "userdata/capture/" + g_controlCapture.front();
                g_controlCaptureAck = g_controlCapture.front();
                g_controlCapture.pop_front();
            }
            else
                g_capDir = "userdata/capture/" + std::to_string(int(captureTimes[nextCapture++]));
            g_capStep = g_capSlots = g_capProbeUsed = 0;
            g_capProbes.clear();
            std::error_code ec;
            std::filesystem::create_directories(g_capDir, ec);
            if (!g_capAtlas.image && !CreateImage(g_capAtlas, VK_FORMAT_R8G8B8A8_UNORM, kThumbCols * kThumbW, kThumbRows * kThumbH,
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D))
                return true;
            BeginFrame();
            VkClearColorValue gray{};
            gray.float32[0] = gray.float32[1] = gray.float32[2] = 0.25f;
            gray.float32[3] = 1.0f;
            ClearImage(g_capAtlas, gray);
            g_capLog = std::fopen((g_capDir + "/steps.txt").c_str(), "w");
            if (g_capLog) g_capState = CaptureState::Active;
        }
        return true;
    }
}
