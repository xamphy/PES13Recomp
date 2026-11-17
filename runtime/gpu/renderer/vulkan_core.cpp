// Vulkan basics: memory, buffers, images, layout transitions, frame slots, GPU timestamps.
// Part of the renderer (see renderer_internal.h for the file map).
#include "renderer_internal.h"

namespace RendererImpl
{
    void WarnOnce(const std::string& what)
    {
        static std::set<std::string> seen;
        if (seen.insert(what).second)
            std::fprintf(stderr, "[render] %s\n", what.c_str());
    }

    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer){
        const auto& mp = g_vk->memory;
        for (uint32_t pass = 0; pass < 2; ++pass)
        {
            const VkMemoryPropertyFlags need = pass == 0 ? (want | prefer) : want;
            for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need)
                    return i;
        }
        return UINT32_MAX;
    }

    // vkAllocateMemory with a log line on failure (the first 20), so running out of GPU memory
    // (iPad: shared with the app's memory limit) shows in the log instead of failing silently.
    bool AllocateMemory(const VkMemoryAllocateInfo& ai, VkDeviceMemory* out, const char* what)
    {
        const VkResult r = ai.memoryTypeIndex == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT
                                                             : vkAllocateMemory(g_dev, &ai, nullptr, out);
        if (r == VK_SUCCESS)
            return true;
        static std::atomic<int> reported{ 0 };
        if (reported.fetch_add(1) < 20)
            std::fprintf(stderr, "[render] %s: allocating %llu KB failed (VkResult %d, memory type %u) | in use: host buffers %llu MB, device buffers %llu MB, images %llu MB in %llu\n",
                what, (unsigned long long)(ai.allocationSize >> 10), int(r), ai.memoryTypeIndex,
                (unsigned long long)(g_mem.hostBuffers >> 20), (unsigned long long)(g_mem.deviceBuffers >> 20),
                (unsigned long long)(g_mem.images >> 20), (unsigned long long)g_mem.imageCount);
        return false;
    }

    void DestroyBuffer(Buffer& b)
    {
        if (b.mapped) vkUnmapMemory(g_dev, b.memory);
        if (b.buffer) vkDestroyBuffer(g_dev, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(g_dev, b.memory, nullptr);
        (b.hostVisible ? g_mem.hostBuffers : g_mem.deviceBuffers) -= b.allocSize;
        b = {};
    }

    bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage)
    {
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = size;
        bi.usage = usage;
        if (vkCreateBuffer(g_dev, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_dev, b.buffer, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!AllocateMemory(ai, &b.memory, "host buffer")) return false;
        vkBindBufferMemory(g_dev, b.buffer, b.memory, 0);
        b.allocSize = req.size;
        b.hostVisible = true;
        g_mem.hostBuffers += req.size;
        void* p = nullptr;
        if (vkMapMemory(g_dev, b.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) return false;
        b.mapped = static_cast<uint8_t*>(p);
        b.size = size;
        return true;
    }

    // Device-local buffer (not mappable).
    bool CreateDeviceBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage)
    {
        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = size;
        bi.usage = usage;
        if (vkCreateBuffer(g_dev, &bi, nullptr, &b.buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_dev, b.buffer, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!AllocateMemory(ai, &b.memory, "device buffer")) return false;
        vkBindBufferMemory(g_dev, b.buffer, b.memory, 0);
        b.allocSize = req.size;
        g_mem.deviceBuffers += req.size;
        b.size = size;
        return true;
    }

    // Only for images no command buffer still in flight uses (see EvictUnusedImages).
    void DestroyImage(Image& img)
    {
        for (auto& [swizzle, view] : img.views) vkDestroyImageView(g_dev, view, nullptr);
        if (img.attachmentView) vkDestroyImageView(g_dev, img.attachmentView, nullptr);
        if (img.image) vkDestroyImage(g_dev, img.image, nullptr);
        if (img.memory) vkFreeMemory(g_dev, img.memory, nullptr);
        if (img.image)
        {
            g_mem.images -= img.allocSize;
            --g_mem.imageCount;
            ++g_mem.imagesFreed;
        }
        img = {};
    }

    bool CreateImage(Image& img, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
        VkImageType type, VkImageViewType viewType, uint32_t layers, uint32_t depth, uint32_t levels){
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.flags = viewType == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ii.imageType = type;
        ii.format = format;
        ii.extent = { w, h, depth };
        ii.mipLevels = levels;
        ii.arrayLayers = layers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        if (vkCreateImage(g_dev, &ii, nullptr, &img.image) != VK_SUCCESS) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g_dev, img.image, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!AllocateMemory(ai, &img.memory, "image")) return false;
        vkBindImageMemory(g_dev, img.image, img.memory, 0);
        img.allocSize = req.size;
        g_mem.images += req.size;
        ++g_mem.imageCount;
        img.format = format;
        img.width = w;
        img.height = h;
        img.layers = layers;
        img.levels = levels;
        img.depth = depth;
        img.viewType = viewType;
        img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        img.aspect = (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                                                           : VK_IMAGE_ASPECT_COLOR_BIT;
        if (usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))
        {
            VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            vi.image = img.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = format;
            vi.subresourceRange = { img.aspect, 0, 1, 0, 1 };
            if (vkCreateImageView(g_dev, &vi, nullptr, &img.attachmentView) != VK_SUCCESS) return false;
        }
        return true;
    }

    // Packed swizzle: 4 x 3 bits of VkComponentSwizzle.
    VkImageView SampledView(Image& img, uint32_t packedSwizzle)
    {
        auto it = img.views.find(packedSwizzle);
        if (it != img.views.end()) return it->second;
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = img.image;
        vi.viewType = img.viewType;
        vi.format = img.format;
        vi.components = { VkComponentSwizzle(packedSwizzle & 7), VkComponentSwizzle((packedSwizzle >> 3) & 7),
            VkComponentSwizzle((packedSwizzle >> 6) & 7), VkComponentSwizzle((packedSwizzle >> 9) & 7) };
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, img.levels, 0, img.layers };
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(g_dev, &vi, nullptr, &view);
        img.views.emplace(packedSwizzle, view);
        return view;
    }

    // Heap offset of guest bytes [start, end) (page-aligned), mapping them if needed; ~0u if the heap is full.
    uint32_t MapVertexRange(uint32_t start, uint32_t end)
    {
        auto lo = g_vertexRanges.upper_bound(start);
        if (lo != g_vertexRanges.begin())
        {
            auto p = std::prev(lo);
            if (p->second.end >= end) return p->second.heap + (start - p->first); // already mapped
            if (p->second.end > start) lo = p;
        }
        uint32_t ns = start, ne = end;
        auto hi = lo;
        for (; hi != g_vertexRanges.end() && hi->first < end; ++hi)
        {
            ns = std::min(ns, hi->first);
            ne = std::max(ne, hi->second.end);
        }
        if (uint64_t(g_vertexHeapTop) + (ne - ns) > g_sharedBytes)
        {
            g_vertexHeapFull = true;
            return ~0u;
        }
        if (lo != hi) ++g_vertexRangeMerges;
        g_vertexRanges.erase(lo, hi);
        const uint32_t heap = g_vertexHeapTop;
        g_vertexHeapTop += ne - ns;
        g_vertexRanges.emplace(ns, VertexRange{ ne, heap });
        // The pages now live at a new heap place: copy them again.
        for (uint32_t p = ns >> 12; p < ne >> 12; ++p)
        {
            g_pagesValid[p >> 6] &= ~(1ull << (p & 63));
            g_pagesCopied[p >> 6] &= ~(1ull << (p & 63));
        }
        return heap + (start - ns);
    }

    // At a frame start (BeginFrame): empty the heap if it is getting full.
    void MaybeResetVertexHeap()
    {
        if (g_vertexIdentity || (!g_vertexHeapFull && g_vertexHeapTop <= g_sharedBytes / 4 * 3)) return;
        g_vertexRanges.clear();
        g_vertexHeapTop = kVertexHeapStart;
        g_vertexHeapFull = false;
        std::fill(g_pagesValid.begin(), g_pagesValid.end(), 0);
        ++g_vertexHeapResets;
    }

    VkAccessFlags AccessFor(VkImageLayout l)
    {
        switch (l)
        {
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT;
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT;
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        case VK_IMAGE_LAYOUT_GENERAL: return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        default: return 0;
        }
    }

    void Transition(Image& img, VkImageLayout to)
    {
        if (img.layout == to) return;
        EndPass("layout change");
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = img.layout;
        b.newLayout = to;
        b.srcAccessMask = AccessFor(img.layout);
        b.dstAccessMask = AccessFor(to);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = { img.aspect, 0, img.levels, 0, img.layers };
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        img.layout = to;
        ++g_barriers;
    }

    void ClearImage(Image& img, const VkClearColorValue& color)
    {
        Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, img.levels, 0, img.layers };
        vkCmdClearColorImage(g_cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    }

    // Waits for a slot's submission and collects its GPU timestamps.
    void WaitSlot(FrameSlot& s)
    {
        if (!s.pending) return;
        const auto t0 = std::chrono::steady_clock::now();
        vkWaitForFences(g_dev, 1, &s.fence, VK_TRUE, UINT64_MAX);
        g_time.fenceWait += uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
        s.pending = false;
        if (s.tsCount > 1)
        {
            std::vector<uint64_t> ts(s.tsCount);
            if (vkGetQueryPoolResults(g_dev, s.timestamps, 0, s.tsCount, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                const double period = g_vk->properties.limits.timestampPeriod / 1e6; // ns -> ms
                for (uint32_t i = 1; i < s.tsCount; ++i)
                {
                    const double ms = double(ts[i] - ts[i - 1]) * period;
                    g_gpuMs[s.tsCategory[i]] += ms;
                    if (s.tsShader[i]) { auto& e = g_shaderGpuMs[s.tsShader[i]]; e.first += ms; ++e.second; }
                }
                g_time.gpuSpanUs += uint64_t(double(ts[s.tsCount - 1] - ts[0]) * period * 1000.0); // first to last timestamp
                for (const PassSpan& p : s.passSpans)
                    if (p.end < s.tsCount && p.begin < p.end)
                    {
                        PassGpu& g = g_passGpu[p.label];
                        g.ms += double(ts[p.end] - ts[p.begin]) * period;
                        ++g.passes;
                        g.draws += p.draws;
                    }
            }
        }
    }

    // Waits until the GPU has finished everything submitted (debug readbacks, shutdown).
    void WaitForGpu()
    {
        for (FrameSlot& s : g_slots) WaitSlot(s);
    }

    void BeginFrame()
    {
        if (g_recording) return;
        g_slot = (g_slot + 1) % kFramesInFlight;
        FrameSlot& s = g_slots[g_slot];
        WaitSlot(s); // the frame that last used this slot (kFramesInFlight frames ago)
        EvictUnusedImages();
        g_cmd = s.cmd;
        g_uploadCmd = s.uploadCmd;
        g_fence = s.fence;
        g_descriptorPool = s.pool;
        g_tsPool = s.timestamps;
        g_tsCategory = s.tsCategory.data();
        g_tsShader = s.tsShader.data();
        g_passSpans = &s.passSpans;
        // Pools that overflowed in the frame just recorded: grow the target to 1.5x what it needed;
        // this slot's buffers are idle now (WaitSlot) and are recreated if smaller than the target.
        for (Pool* p : { &g_uniforms, &g_uploads })
            if (p->failedBytes)
            {
                // Capped: large one-time spikes (texture uploads) use one-off staging buffers instead.
                const VkDeviceSize cap = p == &g_uploads ? VkDeviceSize(192) << 20 : VkDeviceSize(32) << 20;
                p->target = std::min(cap, std::max(p->target, (p->offset + p->failedBytes) * 3 / 2));
                std::printf("[render] per-frame %s pool overflowed by %.1f MB: growing to %.0f MB\n", p == &g_uploads ? "upload" : "uniform",
                    p->failedBytes / 1048576.0, p->target / 1048576.0);
                p->failedBytes = 0;
            }
        auto regrow = [](Buffer& b, const Pool& p, VkBufferUsageFlags usage) {
            if (b.size >= p.target) return;
            DestroyBuffer(b);
            if (!CreateBuffer(b, p.target, usage)) WarnOnce("per-frame pool growth failed");
        };
        regrow(s.uniforms, g_uniforms, kUniformPoolUsage);
        regrow(s.uploads, g_uploads, kUploadPoolUsage);
        g_uniforms.buf = s.uniforms;
        g_uploads.buf = s.uploads;
        vkResetDescriptorPool(g_dev, g_descriptorPool, 0);
        g_textureSetCache.clear();
        g_constantSetCache.clear();
        ResetDepthToColorPool();
        g_uniforms.offset = 0;
        g_uploads.offset = 0;
        std::fill(g_pagesCopied.begin(), g_pagesCopied.end(), 0);
        MaybeResetVertexHeap();
        g_pendingCopies.clear();
        g_rectScratchUsed = 0;
        vkResetCommandBuffer(g_cmd, 0);
        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g_cmd, &bi);
        g_recording = true;
        vkCmdResetQueryPool(g_cmd, g_tsPool, 0, kMaxTimestamps);
        g_tsCount = 0;
        if (g_passSpans) g_passSpans->clear();
        MarkGpu(3, true);
    }

    // Submits the frame. With wait, also waits for the GPU to finish everything (readbacks);
    // otherwise the CPU goes on recording the next frame while the GPU works.
    void EndFrame(bool wait){
        if (!g_recording) return;
        ++g_time.frames;
        const auto tEnd0 = std::chrono::steady_clock::now();
        EndPass("frame end");
        MarkGpu(3, true);
        vkEndCommandBuffer(g_cmd);
        g_uploadsPeak = std::max(g_uploadsPeak, g_uploads.offset);
        g_uniformsPeak = std::max(g_uniformsPeak, g_uniforms.offset);

        // Guest memory uploads for this frame, executed before the frame's commands.
        vkResetCommandBuffer(g_uploadCmd, 0);
        VkCommandBufferBeginInfo ubi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        ubi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(g_uploadCmd, &ubi);
        if (!g_pendingCopies.empty())
        {
            VkMemoryBarrier before{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            before.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            before.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(g_uploadCmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &before, 0, nullptr, 0, nullptr);
            // A page the CPU changed mid-frame is copied again later in the list (newest wins).
            // Regions of one vkCmdCopyBuffer must not overlap: split the list where they would,
            // with a barrier so the later copy lands last.
            std::vector<std::pair<VkDeviceSize, VkDeviceSize>> covered; // [dst, dst + size) in this batch
            size_t batchStart = 0;
            for (size_t i = 0; i <= g_pendingCopies.size(); ++i)
            {
                bool overlap = false;
                if (i < g_pendingCopies.size())
                {
                    const VkBufferCopy& c = g_pendingCopies[i];
                    for (const auto& [b, e] : covered)
                        if (c.dstOffset < e && c.dstOffset + c.size > b) { overlap = true; break; }
                }
                if (i == g_pendingCopies.size() || overlap)
                {
                    if (i > batchStart)
                        vkCmdCopyBuffer(g_uploadCmd, g_uploads.buf.buffer, g_shared.buffer, uint32_t(i - batchStart), g_pendingCopies.data() + batchStart);
                    if (overlap)
                    {
                        VkMemoryBarrier ww{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
                        ww.srcAccessMask = ww.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        vkCmdPipelineBarrier(g_uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &ww, 0, nullptr, 0, nullptr);
                        covered.clear();
                    }
                    batchStart = i;
                }
                if (i < g_pendingCopies.size())
                    covered.emplace_back(g_pendingCopies[i].dstOffset, g_pendingCopies[i].dstOffset + g_pendingCopies[i].size);
            }
            VkMemoryBarrier after{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
            after.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            after.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
            vkCmdPipelineBarrier(g_uploadCmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0, nullptr, 0, nullptr);
        }
        vkEndCommandBuffer(g_uploadCmd);
        const VkCommandBuffer buffers[2] = { g_uploadCmd, g_cmd };
        VkSubmitInfo si{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.commandBufferCount = 2;
        si.pCommandBuffers = buffers;
        vkResetFences(g_dev, 1, &g_fence);
        ScopedTimer timer{ g_time.submitWait };
        auto us = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
            return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count()); };
        const auto tSubmit0 = std::chrono::steady_clock::now();
        g_time.endRecord += us(tEnd0, tSubmit0);
        FrameSlot& slot = g_slots[g_slot];
        Video::GpuSubmitScope gpuAllowed; // iPadOS: waits while the app is in the background
        if (vkQueueSubmit(g_vk->queue, 1, &si, g_fence) == VK_SUCCESS)
        {
            g_time.submit += us(tSubmit0, std::chrono::steady_clock::now());
            slot.pending = true;
            slot.tsCount = g_tsCount;
        }
        else
            WarnOnce("vkQueueSubmit failed");
        g_recording = false;
        if (wait) WaitForGpu();
    }

    void MarkGpu(uint8_t category, bool frameBoundary)
    {
        if (!g_recording || g_tsCount >= kMaxTimestamps) return;
#ifdef __APPLE__
        // Apple GPUs sample counters only at encoder boundaries: MoltenVK ends the current Metal
        // render encoder for every timestamp, so per-draw marks split render passes (costly on
        // the tile-based GPU; the iPad got slower when timestamps started working). Marks inside a
        // render pass are dropped; BeginPass / EndPass mark the pass edges instead (category 3
        // before, 0 after), so the categories still split draws / resolves / uploads / transfers.
        if (!frameBoundary && g_inPass) return;
#else
        (void)frameBoundary;
#endif
        g_tsCategory[g_tsCount] = category;
        g_tsShader[g_tsCount] = category == 0 ? g_markShader : 0;
        vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsPool, g_tsCount++);
    }
}
