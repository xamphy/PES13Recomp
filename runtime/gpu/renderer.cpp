// Vulkan renderer for the command processor (see renderer.h). The per-draw logic follows
// Xenia's VulkanCommandProcessor::IssueDraw / UpdateSystemConstantValues / UpdateBindings
// (host render target path), using Xenia's shader translator and draw_util helpers.
#include "renderer.h"
#include "gpu.h"
#include "d3d_wait.h"
#include "../debug/process_memory.h"
#include "memory_watch.h"
#include "../debug/write_watch.h"

#include <guest_memory.h>
#include <video/vulkan_context.h>
#include <video/presenter.h>
#include <kernel/kernel.h>
#include <kernel/objects.h>
#include <kernel/thread.h>

#include "xenia/base/string_buffer.h"
#include "xenia/gpu/draw_util.h"
#include "xenia/gpu/draw_extent_estimator.h"
#include "xenia/gpu/shader_interpreter.h"
#include "xenia/gpu/register_file.h"
#include "xenia/gpu/registers.h"
#include "xenia/gpu/spirv_shader.h"
#include "xenia/gpu/spirv_shader_translator.h"
#include "xenia/gpu/texture_address.h"
#include "xenia/gpu/texture_info.h"
#include "xenia/gpu/texture_util.h"
#include "xenia/gpu/trace_writer.h"
#include "xenia/gpu/xenos.h"
#include "xenia/memory.h"
#include "xenia/base/xxhash.h"
#include "third_party/glslang/SPIRV/disassemble.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <array>
#include <cstdarg>
#include <filesystem>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <bit>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // VirtualQuery (debug dumps)
#endif

namespace Input { bool QueuePress(const std::string& names, double seconds); }

namespace
{
    using namespace xe::gpu;
    namespace xenos = xe::gpu::xenos;
    using Translator = SpirvShaderTranslator;

    // ------------------------------------------------------------------ basics
    const Video::VulkanContext* g_vk = nullptr;
    VkDevice g_dev = VK_NULL_HANDLE;
    bool g_active = false;
    uint64_t g_frame = 0;

    RegisterFile g_regs;
    xe::Memory g_xeMemory;
    TraceWriter g_trace;
    xe::gpu::DrawExtentEstimator g_extentEstimator(g_regs, g_xeMemory, &g_trace);
    std::unique_ptr<Translator> g_translator;

    constexpr uint32_t kSharedMemorySize = 512u << 20;

    // Debug statistics, printed every 120 frames.
    struct Stats { uint32_t issued, resolves, noVs, noRaster, noColor, noModule, noViewport, noPipeline, badPrim, other; } g_stats{};
    // Time spent per phase (microseconds), printed with the stats.
    struct Timing { uint64_t vertexCopy, vertexBytes, textures, textureUploads, drawTotal, resolve, submitWait, modules, pipelines, newModules, newPipelines, extentEstimate, rectCorners, rectDraws, endRecord, submit, fenceWait, present, gpuSpanUs, frames; } g_time{};
    // GPU timestamps: each mark closes an interval since the previous mark and attributes it
    // to a category (0 draw, 1 resolve, 2 texture upload, 3 other: barriers, pass changes, gaps).
    VkQueryPool g_tsPool = VK_NULL_HANDLE;
#ifdef __APPLE__
    // Metal counter sample buffers hold at most 32 KB (4096 timestamps); a bigger pool makes
    // MoltenVK fall back to emulated timestamps that always read 0.
    constexpr uint32_t kMaxTimestamps = 4096;
#else
    constexpr uint32_t kMaxTimestamps = 16384;
#endif
    constexpr uint32_t kFramesInFlight = 2;
    uint32_t g_tsCount = 0;
    uint8_t* g_tsCategory = nullptr; // the current frame slot's array
    double g_gpuMs[4] = {};
    uint32_t g_passBegins = 0, g_barriers = 0;
    void MarkGpu(uint8_t category);

    struct ScopedTimer
    {
        uint64_t& sink;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~ScopedTimer() { sink += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count(); }
    };
    constexpr uint32_t kRtWidth = 2048, kRtHeight = 2048, kRtMaxHeight = 4096;

    void WarnOnce(const std::string& what)
    {
        static std::set<std::string> seen;
        if (seen.insert(what).second)
            std::fprintf(stderr, "[render] %s\n", what.c_str());
    }

    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer = 0)
    {
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

    // Vulkan memory in use, by kind (bytes as allocated), for the [mem] report.
    struct MemCounters { uint64_t hostBuffers = 0, deviceBuffers = 0, images = 0, imageCount = 0, imagesFreed = 0, poolExhausted = 0; } g_mem;

    // ------------------------------------------------------------------ buffers
    struct Buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* mapped = nullptr;
        VkDeviceSize size = 0;
        VkDeviceSize allocSize = 0;
        bool hostVisible = false;
    };

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
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g_dev, &ai, nullptr, &b.memory) != VK_SUCCESS) return false;
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
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g_dev, &ai, nullptr, &b.memory) != VK_SUCCESS) return false;
        vkBindBufferMemory(g_dev, b.buffer, b.memory, 0);
        b.allocSize = req.size;
        g_mem.deviceBuffers += req.size;
        b.size = size;
        return true;
    }

    // Linear per-frame allocator in a host-visible buffer.
    // Sized near the measured peak; a frame that overflows loses those allocations (logged), and
    // every slot's buffer is then recreated larger at its next BeginFrame (target below).
    struct Pool
    {
        Buffer buf;
        VkDeviceSize offset = 0;
        VkDeviceSize failedBytes = 0; // requested this frame but did not fit
        VkDeviceSize target = 0;      // size the slots' buffers should have (grows after overflows)
        uint8_t* Allocate(VkDeviceSize size, VkDeviceSize align, VkDeviceSize& offsetOut)
        {
            VkDeviceSize o = (offset + align - 1) / align * align;
            if (o + size > buf.size)
            {
                WarnOnce("per-frame upload pool exhausted");
                ++g_mem.poolExhausted;
                failedBytes += size + align;
                return nullptr;
            }
            offset = o + size;
            offsetOut = o;
            return buf.mapped + o;
        }
    };

    // ------------------------------------------------------------------ images
    struct Image
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        uint32_t width = 0, height = 0, layers = 1, depth = 1;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageView attachmentView = VK_NULL_HANDLE; // 2D, identity (render targets)
        std::map<uint32_t, VkImageView> views;      // sampled views by packed swizzle
        VkDeviceSize allocSize = 0;
    };

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
        VkImageType type = VK_IMAGE_TYPE_2D, VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, uint32_t layers = 1, uint32_t depth = 1)
    {
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.flags = viewType == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ii.imageType = type;
        ii.format = format;
        ii.extent = { w, h, depth };
        ii.mipLevels = 1;
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
        if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g_dev, &ai, nullptr, &img.memory) != VK_SUCCESS) return false;
        vkBindImageMemory(g_dev, img.image, img.memory, 0);
        img.allocSize = req.size;
        g_mem.images += req.size;
        ++g_mem.imageCount;
        img.format = format;
        img.width = w;
        img.height = h;
        img.layers = layers;
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
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img.layers };
        VkImageView view = VK_NULL_HANDLE;
        vkCreateImageView(g_dev, &vi, nullptr, &view);
        img.views.emplace(packedSwizzle, view);
        return view;
    }

    constexpr uint32_t PackSwizzle(VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b, VkComponentSwizzle a)
    {
        return uint32_t(r) | (uint32_t(g) << 3) | (uint32_t(b) << 6) | (uint32_t(a) << 9);
    }
    constexpr uint32_t kIdentitySwizzle = PackSwizzle(VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);
    constexpr uint32_t kSwapRBSwizzle = PackSwizzle(VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A);

    // ------------------------------------------------------------------ frame
    VkCommandPool g_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer g_cmd = VK_NULL_HANDLE;
    VkFence g_fence = VK_NULL_HANDLE;
    VkDescriptorPool g_descriptorPool = VK_NULL_HANDLE; // reset every frame
    // Per-frame descriptor set reuse. Sets come from the frame slot's pool and refer to the slot's
    // uniform buffer, so both caches are cleared whenever that pool is reset (BeginFrame).
    std::unordered_map<uint64_t, VkDescriptorSet> g_textureSetCache; // XXH3 of layout + views + samplers
    std::unordered_map<uint32_t, VkDescriptorSet> g_constantSetCache; // (vertex float range, pixel float range)
    bool g_dynamicConstants = true; // constant buffers are UNIFORM_BUFFER_DYNAMIC (one set per frame, offsets per draw)
    uint64_t g_setsAllocated = 0, g_setsReused = 0;
    bool g_recording = false;
    Pool g_uniforms, g_uploads;
    // Guest physical memory mirror in VRAM. Each frame, every 4 KB page a draw reads is
    // copied (at most once per frame) through the upload pool into it, by g_uploadCmd,
    // which is submitted ahead of the frame's command buffer.
    Buffer g_shared;
    VkCommandBuffer g_uploadCmd = VK_NULL_HANDLE;
    std::vector<uint64_t> g_pagesCopied((512u << 20) / 4096 / 64);
    std::vector<uint64_t> g_pageHash((512u << 20) / 4096); // XXH3 of each page as copied this frame
    uint64_t g_pagesRecopied = 0;
    uint64_t g_texHashUs = 0, g_texHashBytes = 0, g_texSetUs = 0, g_constUs = 0; // stats
    std::mutex g_wbStatsMutex;
    std::map<uint64_t, uint32_t> g_wbStats; // synchronous resolve write-backs per (address, size), stats
    // Persistent vertex pages (2026-11-08): a page stays valid in the GPU mirror until the CPU writes
    // it (MemoryWatch page protection records the write) - no per-frame re-copy, no hashing.
    std::vector<uint64_t> g_pagesValid((512u << 20) / 4096 / 64);
    std::vector<uint32_t> g_pageCopyEpoch((512u << 20) / 4096);
    // PES_VB_HASH=1 / control "vb hash|track": the previous scheme (re-copy every frame, XXH3 of reused pages).
    bool g_vbHash = std::getenv("PES_VB_HASH") != nullptr;
    std::vector<VkBufferCopy> g_pendingCopies;
    // Rectangle lists (D3D clears, screen-space quads): the 4th corner of each rectangle is
    // computed on the CPU and the expanded vertices are written to a runtime-owned region of
    // guest physical memory (in the VRAM mirror only), which the draw's fetch constants point at.
    uint32_t g_rectScratch = 0; // offset in g_shared (identity mode: a runtime-owned physical address)
    constexpr uint32_t kRectScratchSize = 1u << 20;
    uint32_t g_rectScratchUsed = 0;

    // Vertex heap (2026-11-09). g_shared used to mirror all 512 MB of guest physical memory at
    // identical offsets, but in a match only ~32 MB of pages ever hold vertex data, spread over
    // ~434 MB. Now each guest range a draw fetches vertices from gets its own place in a compact
    // heap, and the draw's fetch constants (our uploaded copy) point there. Ranges are page-aligned;
    // a range that overlaps existing ones is merged into a new, larger range (re-copied; the old
    // space stays unused until the next reset). The heap is reset at a frame start when it is 3/4
    // full or a draw did not fit (the previous frame's reads are ordered before the next frame's
    // copies by the barrier at the start of g_uploadCmd). PES_VERTEX_IDENTITY=1: old 512 MB mirror.
    const bool g_vertexIdentity = std::getenv("PES_VERTEX_IDENTITY") != nullptr;
    VkDeviceSize g_sharedBytes = 0; // size of g_shared (identity: 512 MB)
    struct VertexRange { uint32_t end; uint32_t heap; }; // guest [start, end) -> heap offset of start
    std::map<uint32_t, VertexRange> g_vertexRanges;      // by guest start address
    constexpr uint32_t kVertexHeapStart = 4096 + kRectScratchSize; // [4 KB, 4 KB + 1 MB): rect scratch
    uint32_t g_vertexHeapTop = kVertexHeapStart;
    bool g_vertexHeapFull = false;
    uint64_t g_vertexHeapResets = 0, g_vertexRangeMerges = 0;

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

    struct RenderTarget;
    // The open render pass (color and/or depth target), if any.
    RenderTarget* g_passColor = nullptr;
    RenderTarget* g_passDepth = nullptr;
    bool g_inPass = false;

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

    void EndPass();

    void Transition(Image& img, VkImageLayout to)
    {
        if (img.layout == to) return;
        EndPass();
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = img.layout;
        b.newLayout = to;
        b.srcAccessMask = AccessFor(img.layout);
        b.dstAccessMask = AccessFor(to);
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = { img.aspect, 0, 1, 0, img.layers };
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        img.layout = to;
        ++g_barriers;
    }

    void ClearImage(Image& img, const VkClearColorValue& color)
    {
        Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img.layers };
        vkCmdClearColorImage(g_cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    }

    void ResetDepthToColorPool();

    // ------------------------------------------------------------------ frames in flight
    // Two frame slots: while the GPU executes frame N, the CP records frame N+1. Each slot owns
    // its command buffers, fence, descriptor pools, timestamp pool and host-visible
    // upload/uniform buffers; BeginFrame switches the globals (g_cmd, g_uploads, ...) to the next
    // slot after waiting for the frame that last used it (two frames back). Everything else is
    // GPU-only and ordered by barriers (a barrier also orders against earlier submissions).
    struct FrameSlot
    {
        VkCommandBuffer cmd = VK_NULL_HANDLE, uploadCmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkQueryPool timestamps = VK_NULL_HANDLE;
        Buffer uniforms, uploads;
        std::vector<uint8_t> tsCategory = std::vector<uint8_t>(kMaxTimestamps);
        uint32_t tsCount = 0;
        bool pending = false; // submitted, not waited for yet
    };
    FrameSlot g_slots[kFramesInFlight];
    constexpr VkBufferUsageFlags kUniformPoolUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    constexpr VkBufferUsageFlags kUploadPoolUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    uint32_t g_slot = 0;
    VkDeviceSize g_uploadsPeak = 0, g_uniformsPeak = 0;

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
                    g_gpuMs[s.tsCategory[i]] += double(ts[i] - ts[i - 1]) * period;
                g_time.gpuSpanUs += uint64_t(double(ts[s.tsCount - 1] - ts[0]) * period * 1000.0); // first to last timestamp
            }
        }
    }

    // Waits until the GPU has finished everything submitted (debug readbacks, shutdown).
    void WaitForGpu()
    {
        for (FrameSlot& s : g_slots) WaitSlot(s);
    }

    void EvictUnusedImages(); // texture cache section

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
        MarkGpu(3);
    }

    // Submits the frame. With wait, also waits for the GPU to finish everything (readbacks);
    // otherwise the CPU goes on recording the next frame while the GPU works.
    void EndFrame(bool wait = false)
    {
        if (!g_recording) return;
        ++g_time.frames;
        const auto tEnd0 = std::chrono::steady_clock::now();
        EndPass();
        MarkGpu(3);
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

    void MarkGpu(uint8_t category)
    {
        if (!g_recording || g_tsCount >= kMaxTimestamps) return;
        g_tsCategory[g_tsCount] = category;
        vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsPool, g_tsCount++);
    }

    // ------------------------------------------------------------------ render targets
    VkFormat ColorRtFormat(xenos::ColorRenderTargetFormat f)
    {
        switch (f)
        {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA: return VK_FORMAT_R8G8B8A8_UNORM;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case xenos::ColorRenderTargetFormat::k_16_16: return VK_FORMAT_R16G16_SNORM;
        case xenos::ColorRenderTargetFormat::k_16_16_16_16: return VK_FORMAT_R16G16B16A16_SNORM;
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
        case xenos::ColorRenderTargetFormat::k_32_FLOAT: return VK_FORMAT_R32_SFLOAT;
        case xenos::ColorRenderTargetFormat::k_32_32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
        default: return VK_FORMAT_R8G8B8A8_UNORM;
        }
    }

    // A color or depth/stencil render target: one image per (EDRAM base, format, pitch, MSAA
    // mode), 1x samples (one texel per guest pixel), in guest pixel coordinates relative to the
    // EDRAM base. The guest addresses EDRAM in tiles (80x16 samples), and surfaces with another
    // pitch / MSAA mode see the same tiles at other pixel positions (D3D clears with 4x MSAA and
    // a narrow pitch), so every EDRAM tile has an owner - the image holding its current
    // content - and tiles are copied between images when another one uses them (Xenia's model).
    struct RenderTarget
    {
        uint32_t base = 0;
        VkFormat format = VK_FORMAT_UNDEFINED;
        bool isDepth = false;
        bool is64bpp = false;
        uint32_t pitchTiles = 1;
        xenos::MsaaSamples msaa = xenos::MsaaSamples::k1X;
        uint32_t tileW = 80, tileH = 16; // tile size in pixels
        uint32_t lastSourceW = 0, lastSourceH = 0; // size of the first texture the last draw into it sampled
        Image img;
        // Depth/stencil: guest format D24FS8 (20e4 float depth), aspect views for the depth -> color transfer.
        bool depthFloat = false;
        VkImageView depthView = VK_NULL_HANDLE, stencilView = VK_NULL_HANDLE;
        uint64_t lastUsedFrame = 0; // eviction (EvictUnusedImages)
        uint32_t maxRow = 0;        // highest pixel row any draw/resolve touched (sizing stats)
        // Logical size (all of EDRAM from the base, used for coordinates/limits) vs the image, which
        // starts small and grows to the rows actually used (EnsureRows).
        uint32_t fullWidth = 0, fullHeight = 0;
        VkImageUsageFlags usage = 0;
        uint32_t grows = 0;
    };
    std::map<std::tuple<uint32_t, VkFormat, uint32_t, uint32_t>, std::unique_ptr<RenderTarget>> g_rts;
    RenderTarget* g_tileOwner[xenos::kEdramTileCount] = {};
    uint64_t g_tileTransfers = 0;
    uint64_t g_rtsEvicted = 0;
    std::map<std::pair<VkFormat, VkFormat>, VkRenderPass> g_renderPasses;               // (color, depth)
    std::map<std::pair<RenderTarget*, RenderTarget*>, VkFramebuffer> g_framebuffers;     // (color, depth)
    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; // also on Apple GPUs (no D24S8 there)

    // Render pass with an optional color attachment (0) and an optional depth/stencil attachment.
    VkRenderPass RenderPassFor(VkFormat color, VkFormat depth)
    {
        auto it = g_renderPasses.find({ color, depth });
        if (it != g_renderPasses.end()) return it->second;
        VkAttachmentDescription a[2]{};
        VkAttachmentReference colorRef{ 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        VkAttachmentReference depthRef{ 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        uint32_t count = 0;
        VkSubpassDescription sp{};
        sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        if (color != VK_FORMAT_UNDEFINED)
        {
            a[count] = {};
            a[count].format = color;
            a[count].samples = VK_SAMPLE_COUNT_1_BIT;
            a[count].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a[count].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a[count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a[count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a[count].initialLayout = a[count].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorRef.attachment = count++;
            sp.colorAttachmentCount = 1;
            sp.pColorAttachments = &colorRef;
        }
        if (depth != VK_FORMAT_UNDEFINED)
        {
            a[count] = {};
            a[count].format = depth;
            a[count].samples = VK_SAMPLE_COUNT_1_BIT;
            a[count].loadOp = a[count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            a[count].storeOp = a[count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
            a[count].initialLayout = a[count].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthRef.attachment = count++;
            sp.pDepthStencilAttachment = &depthRef;
        }
        VkRenderPassCreateInfo ci{ VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
        ci.attachmentCount = count;
        ci.pAttachments = a;
        ci.subpassCount = 1;
        ci.pSubpasses = &sp;
        VkRenderPass pass = VK_NULL_HANDLE;
        vkCreateRenderPass(g_dev, &ci, nullptr, &pass);
        g_renderPasses.emplace(std::make_pair(color, depth), pass);
        return pass;
    }

    // EDRAM tile size in render-target pixels: 80x16 samples (40x16 at 64bpp).
    void TileSize(xenos::MsaaSamples msaa, bool is64bpp, uint32_t& w, uint32_t& h)
    {
        w = (xenos::kEdramTileWidthSamples >> (msaa >= xenos::MsaaSamples::k4X ? 1 : 0)) >> (is64bpp ? 1 : 0);
        h = xenos::kEdramTileHeightSamples >> (msaa >= xenos::MsaaSamples::k2X ? 1 : 0);
    }

    bool Is64bpp(VkFormat f)
    {
        return f == VK_FORMAT_R16G16B16A16_SFLOAT || f == VK_FORMAT_R16G16B16A16_SNORM || f == VK_FORMAT_R32G32_SFLOAT;
    }

    // Whole image to the target's initial value (color 0, depth 1 / stencil 0); leaves it in TRANSFER_DST.
    void ClearRenderTarget(Image& img, bool isDepth)
    {
        Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range{ img.aspect, 0, 1, 0, 1 };
        if (isDepth)
        {
            const VkClearDepthStencilValue clear{ 1.0f, 0 };
            vkCmdClearDepthStencilImage(g_cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        }
        else
        {
            const VkClearColorValue clear{};
            vkCmdClearColorImage(g_cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        }
    }

    // pitchTiles: in tiles of this format (64bpp tiles are half as wide).
    RenderTarget* GetRenderTarget(uint32_t base, VkFormat format, uint32_t pitchTiles, xenos::MsaaSamples msaa)
    {
        pitchTiles = std::max<uint32_t>(pitchTiles, 1);
        const auto key = std::make_tuple(base, format, pitchTiles, uint32_t(msaa));
        auto it = g_rts.find(key);
        if (it != g_rts.end()) { it->second->lastUsedFrame = g_frame; return it->second.get(); }
        auto rt = std::make_unique<RenderTarget>();
        rt->base = base;
        rt->format = format;
        rt->isDepth = format == kDepthFormat;
        rt->is64bpp = Is64bpp(format);
        rt->pitchTiles = pitchTiles;
        rt->msaa = msaa;
        rt->lastUsedFrame = g_frame;
        TileSize(msaa, rt->is64bpp, rt->tileW, rt->tileH);
        // Wide enough for the pitch, tall enough for all of EDRAM from the base (capped).
        const uint32_t width = std::min<uint32_t>(pitchTiles * rt->tileW, kRtWidth);
        const uint32_t height = std::min<uint32_t>((xenos::kEdramTileCount + pitchTiles - 1) / pitchTiles * rt->tileH, kRtMaxHeight);
        // Depth is sampled and RGBA8 color written as a storage image by the depth -> color transfer.
        const VkImageUsageFlags usage = (rt->isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                                     : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
            (format == VK_FORMAT_R8G8B8A8_UNORM ? VK_IMAGE_USAGE_STORAGE_BIT : 0) |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        rt->fullWidth = width;
        rt->fullHeight = height;
        rt->usage = usage;
        // The image starts at 256 rows and grows to what draws/resolves use (EnsureRows): in a match
        // the rows used need ~68 of 273 MB. PES_RT_FULL=1: full height from the start (old behaviour).
        static const bool fullSize = std::getenv("PES_RT_FULL") != nullptr;
        const uint32_t physHeight = fullSize ? height : std::min<uint32_t>(height, 256);
        if (!CreateImage(rt->img, format, width, physHeight, usage, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D))
        {
            WarnOnce("render target creation failed");
            return nullptr;
        }
        ClearRenderTarget(rt->img, rt->isDepth);
        std::printf("[render] %s target: EDRAM base %u, format %d, pitch %u tiles, msaa %u (%ux%u, image %u rows)\n", rt->isDepth ? "depth" : "color",
            base, int(format), pitchTiles, uint32_t(msaa), width, height, physHeight);
        return g_rts.emplace(key, std::move(rt)).first->second.get();
    }

    // Vulkan objects replaced while command buffers in flight may still use them (render-target
    // growth): destroyed by EvictUnusedImages once kFramesInFlight + 1 frames have passed.
    struct Retired
    {
        Image img;
        std::vector<VkImageView> views;
        std::vector<VkFramebuffer> framebuffers;
        Buffer buffer; // one-off staging buffer (StagingAllocate)
        uint64_t frame = 0;
    };
    std::vector<Retired> g_retired;
    uint64_t g_rtGrows = 0;
    uint64_t g_stagingOneOff = 0, g_stagingOneOffBytes = 0;

    // Upload staging: from the frame's pool, or - when it does not fit (a scene's first frame uploads
    // many textures) - a one-off buffer freed once the frame is done, so the pool stays small and
    // nothing is dropped.
    uint8_t* StagingAllocate(VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset)
    {
        // Textures may fill only the first half of the pool: the rest stays free for vertex/index data.
        if (uint8_t* p = g_uploads.offset + size + 16 <= g_uploads.buf.size / 2 ? g_uploads.Allocate(size, 16, offset) : nullptr)
        {
            buffer = g_uploads.buf.buffer;
            return p;
        }
        Retired r;
        r.frame = g_frame;
        if (!CreateBuffer(r.buffer, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return nullptr;
        ++g_stagingOneOff;
        g_stagingOneOffBytes += size;
        buffer = r.buffer.buffer;
        offset = 0;
        uint8_t* p = r.buffer.mapped;
        g_retired.push_back(std::move(r));
        return p;
    }

    // Makes the render target's image at least `rows` tall (capped at its logical height): a taller
    // image is created, cleared, and the old content copied in. Called before any GPU work on the
    // target's rows (AcquireTiles, draws before BeginPass).
    void EnsureRows(RenderTarget* rt, uint32_t rows)
    {
        if (!rt) return;
        rows = std::min(rows, rt->fullHeight);
        if (rows <= rt->img.height) return;
        // Grow by at least half (few regrowths), whole tile rows.
        uint32_t height = std::max(rows, rt->img.height + rt->img.height / 2);
        height = std::min(rt->fullHeight, (height + rt->tileH - 1) / rt->tileH * rt->tileH);

        EndPass();
        Image grown;
        if (!CreateImage(grown, rt->format, rt->img.width, height, rt->usage, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D))
        {
            WarnOnce("render target growth failed");
            return;
        }
        ClearRenderTarget(grown, rt->isDepth);
        Transition(rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER }; // clear -> copy on the new image
        b.srcAccessMask = b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = grown.image;
        b.subresourceRange = { grown.aspect, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = { rt->img.aspect, 0, 0, 1 };
        copy.extent = { rt->img.width, rt->img.height, 1 };
        vkCmdCopyImage(g_cmd, rt->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, grown.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

        Retired r;
        r.frame = g_frame;
        for (VkImageView v : { rt->depthView, rt->stencilView }) if (v) r.views.push_back(v);
        rt->depthView = rt->stencilView = VK_NULL_HANDLE; // recreated on demand (AspectView)
        for (auto fb = g_framebuffers.begin(); fb != g_framebuffers.end();)
            if (fb->first.first == rt || fb->first.second == rt) { r.framebuffers.push_back(fb->second); fb = g_framebuffers.erase(fb); }
            else ++fb;
        r.img = std::move(rt->img);
        g_retired.push_back(std::move(r));
        rt->img = std::move(grown);
        ++rt->grows;
        ++g_rtGrows;
    }

    // ------------------------------------------------------------------ depth -> color tile transfer
    // The guest may alias EDRAM holding depth/stencil as a 32bpp color render target (PES reads
    // depth for its depth-of-field/fog that way). A compute shader writes each pixel's raw EDRAM
    // word (depth24 << 8 | stencil) into the RGBA8 target (runtime/gpu/shaders/depth_to_color.comp).
    const uint32_t kDepthToColorSpv[] =
#include "shaders/depth_to_color.spv.h"
        ;
    struct
    {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool pools[kFramesInFlight] = {}; // one per frame slot
        VkSampler sampler = VK_NULL_HANDLE;
        bool failed = false;
    } g_d2c;
    void ResetDepthToColorPool() { if (g_d2c.pools[g_slot]) vkResetDescriptorPool(g_dev, g_d2c.pools[g_slot], 0); }

    bool InitDepthToColor()
    {
        if (g_d2c.pipeline) return true;
        if (g_d2c.failed) return false;
        g_d2c.failed = true;
        const VkDescriptorSetLayoutBinding b[4] = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo lci{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        lci.bindingCount = 4;
        lci.pBindings = b;
        if (vkCreateDescriptorSetLayout(g_dev, &lci, nullptr, &g_d2c.setLayout) != VK_SUCCESS) return false;
        const VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 24 };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &g_d2c.setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &pcr;
        if (vkCreatePipelineLayout(g_dev, &pli, nullptr, &g_d2c.layout) != VK_SUCCESS) return false;
        VkShaderModuleCreateInfo mci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mci.codeSize = sizeof(kDepthToColorSpv);
        mci.pCode = kDepthToColorSpv;
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(g_dev, &mci, nullptr, &module) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo cpi{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpi.stage.module = module;
        cpi.stage.pName = "main";
        cpi.layout = g_d2c.layout;
        const VkResult r = vkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, nullptr, &g_d2c.pipeline);
        vkDestroyShaderModule(g_dev, module, nullptr);
        if (r != VK_SUCCESS) return false;
        const VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * 256 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 256 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 },
        };
        VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 256;
        dpi.poolSizeCount = 3;
        dpi.pPoolSizes = sizes;
        for (VkDescriptorPool& p : g_d2c.pools)
            if (vkCreateDescriptorPool(g_dev, &dpi, nullptr, &p) != VK_SUCCESS) return false;
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(g_dev, &si, nullptr, &g_d2c.sampler) != VK_SUCCESS) return false;
        g_d2c.failed = false;
        std::printf("[render] depth -> color tile transfer pipeline ready\n");
        return true;
    }

    VkImageView AspectView(RenderTarget* rt, VkImageAspectFlags aspect)
    {
        VkImageView& v = aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? rt->depthView : rt->stencilView;
        if (v) return v;
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = rt->img.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = rt->format;
        vi.subresourceRange = { aspect, 0, 1, 0, 1 };
        vkCreateImageView(g_dev, &vi, nullptr, &v);
        return v;
    }

    // tiles: (src x, src y, dst x, dst y) in pixels, tileW x tileH each.
    void DepthToColor(RenderTarget* from, RenderTarget* to, const std::vector<uint32_t>& tiles, uint32_t tileW, uint32_t tileH)
    {
        if (tiles.empty() || !InitDepthToColor()) return;
        VkDeviceSize offset = 0;
        uint8_t* p = g_uploads.Allocate(tiles.size() * 4, 256, offset);
        if (!p) return;
        std::memcpy(p, tiles.data(), tiles.size() * 4);
        VkDescriptorSetAllocateInfo ai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = g_d2c.pools[g_slot];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g_d2c.setLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (vkAllocateDescriptorSets(g_dev, &ai, &set) != VK_SUCCESS) { WarnOnce("depth -> color: descriptor pool exhausted"); return; }
        Transition(from->img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        Transition(to->img, VK_IMAGE_LAYOUT_GENERAL);
        const VkDescriptorImageInfo depthInfo{ g_d2c.sampler, AspectView(from, VK_IMAGE_ASPECT_DEPTH_BIT), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo stencilInfo{ g_d2c.sampler, AspectView(from, VK_IMAGE_ASPECT_STENCIL_BIT), VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo dstInfo{ VK_NULL_HANDLE, to->img.attachmentView, VK_IMAGE_LAYOUT_GENERAL };
        const VkDescriptorBufferInfo bufInfo{ g_uploads.buf.buffer, offset, tiles.size() * 4 };
        VkWriteDescriptorSet w[4];
        for (uint32_t i = 0; i < 4; ++i)
        {
            w[i] = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
        }
        w[0].descriptorType = w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &depthInfo;
        w[1].pImageInfo = &stencilInfo;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[2].pImageInfo = &dstInfo;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[3].pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(g_dev, 4, w, 0, nullptr);
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_d2c.pipeline);
        vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_d2c.layout, 0, 1, &set, 0, nullptr);
        // PES_D2C_ORDER=old (A/B): the pre-2026-11-08 byte order (red = depth high byte, alpha = stencil).
        static const uint32_t oldOrder = [] { const char* e = std::getenv("PES_D2C_ORDER"); return e && !std::strcmp(e, "old") ? 1u : 0u; }();
        const uint32_t pc[6] = { tileW, tileH, from->depthFloat ? 1u : 0u, oldOrder, from->tileW, from->tileH };
        vkCmdPushConstants(g_cmd, g_d2c.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
        vkCmdDispatch(g_cmd, (tileW + 7) / 8, (tileH + 7) / 8, uint32_t(tiles.size() / 4));
    }

    void CaptureNote(const char* fmt, ...); // a line in the capture step log (if capturing)
    struct Image;
    void DumpImage(Image& img, uint32_t width, uint32_t height, const std::string& path, bool swapRedBlue);

    // Makes rt the owner of the EDRAM tiles under the rectangle (rt pixels), first copying in
    // the content of tiles another render target owns (if the formats allow it).
    // Tile transfer regions -> fewer, larger rectangles: tiles adjacent in both images (same pitch,
    // the usual case) merge into row spans, and identical spans of consecutive rows into blocks.
    struct TileRect { int32_t sx, sy, sw, sh, dx, dy, dw, dh; };
    void MergeTileRects(std::vector<TileRect>& v)
    {
        if (v.size() < 2) return;
        std::vector<TileRect> rows;
        for (const TileRect& t : v)
        {
            if (!rows.empty())
            {
                TileRect& p = rows.back();
                if (t.sy == p.sy && t.dy == p.dy && t.sh == p.sh && t.dh == p.dh && t.sx == p.sx + p.sw && t.dx == p.dx + p.dw)
                {
                    p.sw += t.sw;
                    p.dw += t.dw;
                    continue;
                }
            }
            rows.push_back(t);
        }
        std::vector<TileRect> blocks;
        std::map<std::tuple<int32_t, int32_t, int32_t, int32_t>, size_t> open; // (sx, dx, sw, dw) -> block
        for (const TileRect& t : rows)
        {
            const auto key = std::make_tuple(t.sx, t.dx, t.sw, t.dw);
            auto it = open.find(key);
            if (it != open.end())
            {
                TileRect& b = blocks[it->second];
                if (t.sy == b.sy + b.sh && t.dy == b.dy + b.dh)
                {
                    b.sh += t.sh;
                    b.dh += t.dh;
                    continue;
                }
            }
            open[key] = blocks.size();
            blocks.push_back(t);
        }
        v = std::move(blocks);
    }
    void MergeRegions(std::vector<VkImageCopy>& v)
    {
        if (v.size() < 2) return;
        std::vector<TileRect> t;
        t.reserve(v.size());
        for (const auto& c : v)
            t.push_back({ c.srcOffset.x, c.srcOffset.y, int32_t(c.extent.width), int32_t(c.extent.height),
                          c.dstOffset.x, c.dstOffset.y, int32_t(c.extent.width), int32_t(c.extent.height) });
        MergeTileRects(t);
        const VkImageCopy proto = v[0];
        v.clear();
        for (const auto& r : t)
        {
            VkImageCopy c = proto;
            c.srcOffset = { r.sx, r.sy, 0 };
            c.dstOffset = { r.dx, r.dy, 0 };
            c.extent = { uint32_t(r.sw), uint32_t(r.sh), 1 };
            v.push_back(c);
        }
    }
    void MergeRegions(std::vector<VkImageBlit>& v)
    {
        if (v.size() < 2) return;
        std::vector<TileRect> t;
        t.reserve(v.size());
        for (const auto& b : v)
            t.push_back({ b.srcOffsets[0].x, b.srcOffsets[0].y, b.srcOffsets[1].x - b.srcOffsets[0].x, b.srcOffsets[1].y - b.srcOffsets[0].y,
                          b.dstOffsets[0].x, b.dstOffsets[0].y, b.dstOffsets[1].x - b.dstOffsets[0].x, b.dstOffsets[1].y - b.dstOffsets[0].y });
        MergeTileRects(t);
        const VkImageBlit proto = v[0];
        v.clear();
        for (const auto& r : t)
        {
            VkImageBlit b = proto;
            b.srcOffsets[0] = { r.sx, r.sy, 0 };
            b.srcOffsets[1] = { r.sx + r.sw, r.sy + r.sh, 1 };
            b.dstOffsets[0] = { r.dx, r.dy, 0 };
            b.dstOffsets[1] = { r.dx + r.dw, r.dy + r.dh, 1 };
            v.push_back(b);
        }
    }

    // noCopy: tiles entirely inside it are about to be completely overwritten (a clear): take
    // ownership without copying their old content.
    uint64_t g_tilesSkipped = 0;
    void AcquireTiles(RenderTarget* rt, int32_t x, int32_t y, uint32_t w, uint32_t h, const VkRect2D* noCopy = nullptr)
    {
        if (!rt || !w || !h || x < 0 || y < 0) return;
        rt->maxRow = std::max(rt->maxRow, uint32_t(y) + h);
        const uint32_t c0 = uint32_t(x) / rt->tileW, r0 = uint32_t(y) / rt->tileH;
        const uint32_t c1 = std::min((uint32_t(x) + w - 1) / rt->tileW, rt->pitchTiles - 1);
        const uint32_t r1 = std::min((uint32_t(y) + h - 1) / rt->tileH, rt->fullHeight / rt->tileH - 1);
        EnsureRows(rt, (r1 + 1) * rt->tileH);
        struct Pending { RenderTarget* from; std::vector<VkImageCopy> copies; std::vector<VkImageBlit> blits; };
        std::vector<Pending> pending;
        std::vector<std::pair<RenderTarget*, std::vector<uint32_t>>> d2c; // depth -> color tiles per source
        static int depthBlits = -1; // depth/stencil blits are optional in Vulkan
        for (uint32_t r = r0; r <= r1; ++r)
            for (uint32_t c = c0; c <= c1; ++c)
            {
                const uint32_t local = r * rt->pitchTiles + c;
                if (local >= xenos::kEdramTileCount) break;
                const uint32_t e = (rt->base + local) & (xenos::kEdramTileCount - 1);
                RenderTarget* o = g_tileOwner[e];
                g_tileOwner[e] = rt;
                if (!o || o == rt) continue;
                if (noCopy)
                {
                    const int32_t tx = int32_t(c * rt->tileW), ty = int32_t(r * rt->tileH);
                    if (tx >= noCopy->offset.x && ty >= noCopy->offset.y &&
                        tx + int32_t(rt->tileW) <= noCopy->offset.x + int32_t(noCopy->extent.width) &&
                        ty + int32_t(rt->tileH) <= noCopy->offset.y + int32_t(noCopy->extent.height))
                    {
                        ++g_tilesSkipped;
                        continue;
                    }
                }
                const bool depthToColor = o->isDepth && !rt->isDepth;
                if (!depthToColor && (o->isDepth != rt->isDepth || o->is64bpp != rt->is64bpp)) continue;
                const uint32_t lo = (e - o->base) & (xenos::kEdramTileCount - 1);
                const int32_t sx = int32_t(lo % o->pitchTiles * o->tileW), sy = int32_t(lo / o->pitchTiles * o->tileH);
                const int32_t dx = int32_t(c * rt->tileW), dy = int32_t(r * rt->tileH);
                if (uint32_t(sx) + o->tileW > o->img.width || uint32_t(sy) + o->tileH > o->img.height ||
                    uint32_t(dx) + rt->tileW > rt->img.width || uint32_t(dy) + rt->tileH > rt->img.height)
                    continue;
                if (depthToColor)
                {
                    // Raw depth/stencil words as 32bpp color (another MSAA mode: nearest sample).
                    if (rt->format != VK_FORMAT_R8G8B8A8_UNORM) continue;
                    auto q = std::find_if(d2c.begin(), d2c.end(), [&](const auto& x) { return x.first == o; });
                    if (q == d2c.end()) { d2c.push_back({ o, {} }); q = d2c.end() - 1; }
                    q->second.insert(q->second.end(), { uint32_t(sx), uint32_t(sy), uint32_t(dx), uint32_t(dy) });
                    continue;
                }
                const bool scaled = o->tileW != rt->tileW || o->tileH != rt->tileH;
                if (scaled && rt->isDepth)
                {
                    if (depthBlits < 0)
                    {
                        VkFormatProperties fp{};
                        vkGetPhysicalDeviceFormatProperties(g_vk->physical, kDepthFormat, &fp);
                        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
                        depthBlits = (fp.optimalTilingFeatures & need) == need;
                        std::printf("[render] depth/stencil blits %ssupported\n", depthBlits ? "" : "NOT ");
                    }
                    if (!depthBlits) continue;
                }
                auto p = std::find_if(pending.begin(), pending.end(), [&](const Pending& q) { return q.from == o; });
                if (p == pending.end()) { pending.push_back({ o }); p = pending.end() - 1; }
                if (scaled)
                {
                    VkImageBlit b{};
                    b.srcSubresource = { o->img.aspect, 0, 0, 1 };
                    b.dstSubresource = { rt->img.aspect, 0, 0, 1 };
                    b.srcOffsets[0] = { sx, sy, 0 };
                    b.srcOffsets[1] = { sx + int32_t(o->tileW), sy + int32_t(o->tileH), 1 };
                    b.dstOffsets[0] = { dx, dy, 0 };
                    b.dstOffsets[1] = { dx + int32_t(rt->tileW), dy + int32_t(rt->tileH), 1 };
                    p->blits.push_back(b);
                }
                else
                {
                    VkImageCopy cp{};
                    cp.srcSubresource = { o->img.aspect, 0, 0, 1 };
                    cp.dstSubresource = { rt->img.aspect, 0, 0, 1 };
                    cp.srcOffset = { sx, sy, 0 };
                    cp.dstOffset = { dx, dy, 0 };
                    cp.extent = { rt->tileW, rt->tileH, 1 };
                    p->copies.push_back(cp);
                }
            }
        for (auto& [from, tiles] : d2c)
        {
            DepthToColor(from, rt, tiles, rt->tileW, rt->tileH);
            CaptureNote("      tiles: %zu depth -> color from depth base %u pitch %u msaa %u -> color base %u pitch %u msaa %u", tiles.size() / 4, from->base, from->pitchTiles, uint32_t(from->msaa), rt->base, rt->pitchTiles, uint32_t(rt->msaa));
            g_tileTransfers += tiles.size() / 4;
            static uint32_t logs = 0;
            if (logs++ < 8)
                std::printf("[render] f%llu tiles: %zu depth -> color from depth base %u pitch %u -> color base %u pitch %u msaa %u\n",
                    (unsigned long long)g_frame, tiles.size() / 4, from->base, from->pitchTiles, rt->base, rt->pitchTiles, uint32_t(rt->msaa));
        }
        for (auto& p : pending)
        {
            g_tileTransfers += p.copies.size() + p.blits.size(); // tiles (before merging)
            MergeRegions(p.copies);
            MergeRegions(p.blits);
            Transition(p.from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Transition(rt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            // Copies need size-compatible formats (same bytes per texel - true here); blits convert.
            if (!p.copies.empty())
                vkCmdCopyImage(g_cmd, p.from->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rt->img.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(p.copies.size()), p.copies.data());
            if (!p.blits.empty())
                vkCmdBlitImage(g_cmd, p.from->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rt->img.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(p.blits.size()), p.blits.data(), VK_FILTER_NEAREST);
            CaptureNote("      tiles: %zu copied + %zu scaled from %s base %u pitch %u msaa %u -> base %u pitch %u msaa %u", p.copies.size(), p.blits.size(), rt->isDepth ? "depth" : "color", p.from->base, p.from->pitchTiles, uint32_t(p.from->msaa), rt->base, rt->pitchTiles, uint32_t(rt->msaa));
            // PES_LOG_TILES=<seconds>: log 120 transfers from that time (default: the first 16).
            static uint32_t logs = 0;
            static const double logFrom = [] { const char* e = std::getenv("PES_LOG_TILES"); return e ? std::atof(e) : -1.0; }();
            static const auto logStart = std::chrono::steady_clock::now();
            const bool timeOk = logFrom < 0 || std::chrono::duration<double>(std::chrono::steady_clock::now() - logStart).count() >= logFrom;
            if (timeOk && logs++ < (logFrom < 0 ? 16u : 120u))
                std::printf("[render] f%llu tiles: %zu copied + %zu scaled from %s base %u pitch %u msaa %u -> base %u pitch %u msaa %u\n",
                    (unsigned long long)g_frame, p.copies.size(), p.blits.size(), rt->isDepth ? "depth" : "color",
                    p.from->base, p.from->pitchTiles, uint32_t(p.from->msaa), rt->base, rt->pitchTiles, uint32_t(rt->msaa));
        }
    }

    void EndPass()
    {
        if (!g_inPass) return;
        vkCmdEndRenderPass(g_cmd);
        g_inPass = false;
        g_passColor = g_passDepth = nullptr;
    }

    // Framebuffer size: the smaller of the attachments.
    VkExtent2D PassExtent(const RenderTarget* color, const RenderTarget* depth)
    {
        VkExtent2D e{ kRtWidth, kRtMaxHeight };
        for (const RenderTarget* rt : { color, depth })
            if (rt) e = { std::min(e.width, rt->img.width), std::min(e.height, rt->img.height) };
        return e;
    }

    void BeginPass(RenderTarget* color, RenderTarget* depth)
    {
        if (g_inPass && g_passColor == color && g_passDepth == depth) return;
        EndPass();
        if (color) Transition(color->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (depth) Transition(depth->img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        const VkRenderPass pass = RenderPassFor(color ? color->format : VK_FORMAT_UNDEFINED, depth ? depth->format : VK_FORMAT_UNDEFINED);
        const VkExtent2D extent = PassExtent(color, depth);
        VkFramebuffer& fb = g_framebuffers[{ color, depth }];
        if (!fb)
        {
            VkImageView views[2];
            uint32_t n = 0;
            if (color) views[n++] = color->img.attachmentView;
            if (depth) views[n++] = depth->img.attachmentView;
            VkFramebufferCreateInfo fi{ VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
            fi.renderPass = pass;
            fi.attachmentCount = n;
            fi.pAttachments = views;
            fi.width = extent.width;
            fi.height = extent.height;
            fi.layers = 1;
            vkCreateFramebuffer(g_dev, &fi, nullptr, &fb);
        }
        VkRenderPassBeginInfo bi{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        bi.renderPass = pass;
        bi.framebuffer = fb;
        bi.renderArea = { { 0, 0 }, extent };
        vkCmdBeginRenderPass(g_cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        g_inPass = true;
        g_passColor = color;
        g_passDepth = depth;
        ++g_passBegins;
    }
    // ------------------------------------------------------------------ resolved surfaces
    // A resolve destination: guest texture memory [base, base + size), kept on the GPU.
    struct Surface
    {
        uint32_t base = 0, pitch = 0, height = 0, bytesPerPixel = 4, size = 0;
        bool rbSwapped = false; // last resolve used copy_dest_swap (memory has red/blue exchanged)
        bool isDepth = false;   // depth copy (R32_SFLOAT depth for k_24_8 fetches)
        std::vector<uint8_t> snapshot; // guest bytes sampled at the last resolve (see SnapshotSurface)
        uint64_t generation = 0;       // bumped at every resolve into this surface (composed textures, below)
        uint64_t writtenBackGen = 0;   // generation whose content was stored into guest memory (WriteBackSurface)
        uint32_t resolveEpoch = 0, checkedEpoch = 0; // MemoryWatch epoch at the last resolve / last clean check
        uint64_t lastResolveFrame = 0; // frame of the last resolve into it (one-off vs per-frame resolves)
        uint64_t retiredFrame = 0;     // first eviction sweep that saw it retired (base 0xFFFFFFFF)
        uint64_t lastSampledFrame = 0; // last frame a texture fetch used it (stats / eviction)
        bool mirror = false;           // this frame's resolves into it are stored to guest memory
        Image img;
        // Copies of the top-left fetch-sized region (see SurfaceForFetchSize), by width << 16 | height.
        struct Crop { Image img; uint64_t generation = 0; uint64_t lastUsedFrame = 0; };
        std::map<uint32_t, Crop> crops;
    };
    std::vector<std::unique_ptr<Surface>> g_surfaces;
    uint64_t g_surfaceEpoch = 0; // bumped when a surface is created or retired (address index below)

    Surface* FindSurfaceContaining(uint32_t address, uint32_t pitch, uint32_t bytesPerPixel, VkFormat format)
    {
        for (auto& s : g_surfaces)
            if (address >= s->base && address < s->base + s->size && s->pitch == pitch && s->bytesPerPixel == bytesPerPixel &&
                s->img.format == format)
                return s.get();
        return nullptr;
    }

    // The resolve destination surface covering [base, ...) with this pitch / texel size / host
    // format, created if needed. A surface of another format at the same base is retired (not
    // destroyed: the current command buffer may still use it).
    Surface* GetResolveSurface(uint32_t destBase, uint32_t destPitch, uint32_t destHeight, uint32_t bpp, VkFormat format)
    {
        if (Surface* s = FindSurfaceContaining(destBase, destPitch, bpp, format)) return s;
        if (!destPitch || !destHeight) return nullptr;
        for (auto& old : g_surfaces)
            if (old->base == destBase) { old->base = 0xFFFFFFFFu; ++g_surfaceEpoch; }
        auto ns = std::make_unique<Surface>();
        ns->base = destBase;
        ns->pitch = destPitch;
        ns->height = destHeight;
        ns->bytesPerPixel = bpp;
        ns->size = destPitch * destHeight * bpp;
        if (!CreateImage(ns->img, format, destPitch, destHeight,
                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        {
            WarnOnce("surface creation failed");
            return nullptr;
        }
        ClearImage(ns->img, VkClearColorValue{});
        std::printf("[render] surface 0x%08X: %ux%u, %u bytes/pixel, format %d\n", destBase, destPitch, destHeight, bpp, int(format));
        Surface* s = ns.get();
        g_surfaces.push_back(std::move(ns));
        ++g_surfaceEpoch;
        return s;
    }

    // Destination pixel of a resolve inside its surface (the copy base may be a later 32x32 tile).
    void ResolveDestOffset(const Surface* s, uint32_t destBase, const draw_util::ResolveInfo& info, int32_t& dstX, int32_t& dstY)
    {
        const uint32_t tileBytes = 32 * 32 * s->bytesPerPixel, tilesPerRow = std::max<uint32_t>(s->pitch / 32, 1);
        const uint32_t tileIndex = (destBase - s->base) / tileBytes;
        dstX = int32_t((tileIndex % tilesPerRow) * 32 + info.copy_dest_coordinate_info.offset_x_div_8 * 8);
        dstY = int32_t((tileIndex / tilesPerRow) * 32 + info.copy_dest_coordinate_info.offset_y_div_8 * 8);
    }

    Surface* FindSurfaceAt(uint32_t address)
    {
        for (auto& s : g_surfaces)
            if (s->base == address)
                return s.get();
        return nullptr;
    }

    // Resolves keep their result on the GPU only; guest memory under a surface keeps whatever it
    // held. When the game reuses that memory (loads a skin/kit texture there), the surface is
    // stale. A sample of the guest bytes is kept at every resolve; if the CPU changed them, the
    // surface is retired and the texture is read from guest memory again.
    constexpr uint32_t kSnapshotSamples = 16, kSnapshotBytes = 64;
    uint64_t g_surfaceGeneration = 0;
    void SnapshotSurface(Surface* s)
    {
        s->generation = ++g_surfaceGeneration;
        // From now on any write to its memory (guest or host) is recorded (MemoryWatch).
        s->resolveEpoch = MemoryWatch::Epoch();
        MemoryWatch::Protect(s->base, s->size);
        s->snapshot.resize(kSnapshotSamples * kSnapshotBytes);
        const uint32_t span = s->size > kSnapshotBytes ? s->size - kSnapshotBytes : 0;
        for (uint32_t i = 0; i < kSnapshotSamples; ++i)
        {
            const uint32_t at = s->base + span / (kSnapshotSamples - 1) * i;
            if (at + kSnapshotBytes > kSharedMemorySize) break;
            std::memcpy(s->snapshot.data() + i * kSnapshotBytes, g_xeMemory.TranslatePhysical(at), kSnapshotBytes);
        }
    }
    bool SurfaceStillInMemory(Surface* s)
    {
        // Writes recorded by the page watch since the resolve (exact), checked only when there were new ones.
        const uint32_t epoch = MemoryWatch::Epoch();
        if (s->checkedEpoch != epoch)
        {
            if (MemoryWatch::WrittenSince(s->base, s->size, s->resolveEpoch)) return false;
            s->checkedEpoch = epoch;
        }
        // The sparse snapshot comparison predates the page watch: it misses writes, and it would
        // take the renderer's own CPU-view conversion (PrepareResolveForCpu) for a rewrite.
        // PES_SNAPSHOT_CHECK=1 brings it back (A/B).
        static const bool snapshotCheck = std::getenv("PES_SNAPSHOT_CHECK") != nullptr;
        if (!snapshotCheck || s->snapshot.empty()) return true;
        const uint32_t span = s->size > kSnapshotBytes ? s->size - kSnapshotBytes : 0;
        for (uint32_t i = 0; i < kSnapshotSamples; ++i)
        {
            const uint32_t at = s->base + span / (kSnapshotSamples - 1) * i;
            if (at + kSnapshotBytes > kSharedMemorySize) break;
            if (std::memcmp(s->snapshot.data() + i * kSnapshotBytes, g_xeMemory.TranslatePhysical(at), kSnapshotBytes) != 0)
                return false;
        }
        return true;
    }
    uint64_t g_surfacesRetired = 0;
    uint64_t g_surfacesEvicted = 0;

    // The surface a texture fetch reads, if any: a resolve result at that address whose texel
    // layout fits the fetch (no block-compressed formats; depth formats <-> depth surfaces) and
    // whose memory the CPU hasn't overwritten since.
    // PES_STRICT_SURFACE_EXTENT=<seconds> (diagnostic, from ChatGPT's gpt-surface-extent.patch):
    // from that time on, a surface smaller than the fetch is not used for it (a small resolve at
    // an address later reused by a bigger texture). Switched on in Present (same-run A/B).
    bool g_strictExtent = false;
    uint64_t g_extentRejects = 0;

    Surface* FindSurfaceForTexture(uint32_t base, const xenos::xe_gpu_texture_fetch_t& fetch)
    {
        Surface* s = FindSurfaceAt(base);
        if (!s) return nullptr;
        const auto format = fetch.format;
        const uint32_t fw = uint32_t(fetch.size_2d.width) + 1, fh = uint32_t(fetch.size_2d.height) + 1;
        if (fw > s->pitch || fh > s->height)
        {
            static uint32_t extentLogs = 0;
            if (extentLogs++ < 32)
                std::printf("[surface-extent] %s 0x%08X: fetch %ux%u fmt %u, surface %ux%u\n", g_strictExtent ? "reject" : "mismatch", base,
                    fw, fh, uint32_t(format), s->pitch, s->height);
            if (g_strictExtent) { ++g_extentRejects; return nullptr; }
        }
        const bool depthFetch = format == xenos::TextureFormat::k_24_8 || format == xenos::TextureFormat::k_24_8_FLOAT;
        const bool depthSurface = s->img.format == VK_FORMAT_R32_SFLOAT && s->isDepth;
        if (depthFetch != depthSurface) return nullptr;
        if (!depthFetch)
        {
            const FormatInfo* fi = FormatInfo::Get(format);
            if (!fi || fi->block_width != 1 || fi->block_height != 1 || fi->bits_per_pixel / 8 != s->bytesPerPixel) return nullptr;
        }
        if (!SurfaceStillInMemory(s))
        {
            s->base = 0xFFFFFFFFu; // retired (the command buffer may still use the image)
            ++g_surfaceEpoch;
            ++g_surfacesRetired;
            static uint32_t logs = 0;
            if (logs++ < 10) std::printf("[render] surface retired: guest memory rewritten (texture fmt %u at 0x%08X)\n", uint32_t(format), base);
            return nullptr;
        }
        return s;
    }

    // ------------------------------------------------------------------ guest textures
    struct HostFormat
    {
        VkFormat format;
        uint32_t swizzle; // host swizzle applied before the guest swizzle
    };

    // BC (DXT1/3/5) textures are sampled directly when the device supports BC; otherwise (some
    // Apple GPU / iPadOS combinations) they are decoded to RGBA8 on the CPU at upload.
    // PES_FORCE_BC_DECODE=1 forces the decode path (to test it on a PC). Set in Renderer::Init.
    bool g_bcDecode = false;

    bool IsBC(xenos::TextureFormat f)
    {
        return f == xenos::TextureFormat::k_DXT1 || f == xenos::TextureFormat::k_DXT2_3 || f == xenos::TextureFormat::k_DXT4_5;
    }

    // One 4x4 BC1 color block (8 bytes, little-endian) -> 16 RGBA8 pixels. BC2/BC3 color blocks always
    // use the 4-color mode (fourColor = true).
    void DecodeBC1Color(const uint8_t* b, uint8_t* rgba, bool fourColor)
    {
        const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
        uint8_t pal[4][4];
        auto expand = [](uint16_t c, uint8_t* p) {
            const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
            p[0] = uint8_t((r << 3) | (r >> 2));
            p[1] = uint8_t((g << 2) | (g >> 4));
            p[2] = uint8_t((bl << 3) | (bl >> 2));
            p[3] = 255;
        };
        expand(c0, pal[0]);
        expand(c1, pal[1]);
        if (fourColor || c0 > c1)
            for (int i = 0; i < 3; ++i)
            {
                pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i] + 1) / 3);
                pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i] + 1) / 3);
            }
        else
            for (int i = 0; i < 3; ++i)
            {
                pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
                pal[3][i] = 0;
            }
        pal[2][3] = 255;
        pal[3][3] = (fourColor || c0 > c1) ? 255 : 0; // 3-color mode: index 3 = transparent black
        const uint32_t idx = uint32_t(b[4] | b[5] << 8 | b[6] << 16 | uint32_t(b[7]) << 24);
        for (int p = 0; p < 16; ++p)
            std::memcpy(rgba + p * 4, pal[(idx >> (2 * p)) & 3], 4);
    }

    // One block of a BC format -> 16 RGBA8 pixels (row-major within the 4x4 block).
    void DecodeBCBlock(xenos::TextureFormat f, const uint8_t* b, uint8_t* rgba)
    {
        if (f == xenos::TextureFormat::k_DXT1)
        {
            DecodeBC1Color(b, rgba, false);
            return;
        }
        DecodeBC1Color(b + 8, rgba, true);
        if (f == xenos::TextureFormat::k_DXT2_3) // explicit 4-bit alpha
        {
            for (int p = 0; p < 16; ++p)
            {
                const uint32_t a = (b[p / 2] >> ((p & 1) * 4)) & 15;
                rgba[p * 4 + 3] = uint8_t(a * 17);
            }
            return;
        }
        // BC3 alpha: two 8-bit endpoints + 16 3-bit indices.
        const uint32_t a0 = b[0], a1 = b[1];
        uint8_t pal[8] = { uint8_t(a0), uint8_t(a1) };
        if (a0 > a1)
            for (uint32_t i = 1; i < 7; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1 + 3) / 7);
        else
        {
            for (uint32_t i = 1; i < 5; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1 + 2) / 5);
            pal[6] = 0;
            pal[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int p = 0; p < 16; ++p)
            rgba[p * 4 + 3] = pal[(bits >> (3 * p)) & 7];
    }

    bool GetHostFormat(xenos::TextureFormat f, HostFormat& out)
    {
        if (g_bcDecode && IsBC(f))
        {
            out = { VK_FORMAT_R8G8B8A8_UNORM, kIdentitySwizzle };
            return true;
        }
        using S = VkComponentSwizzle;
        constexpr S R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G, B = VK_COMPONENT_SWIZZLE_B, A = VK_COMPONENT_SWIZZLE_A;
        constexpr S ONE = VK_COMPONENT_SWIZZLE_ONE;
        switch (f)
        {
        case xenos::TextureFormat::k_8: out = { VK_FORMAT_R8_UNORM, PackSwizzle(R, R, R, R) }; return true;
        case xenos::TextureFormat::k_8_8: out = { VK_FORMAT_R8G8_UNORM, PackSwizzle(R, G, G, G) }; return true;
        case xenos::TextureFormat::k_8_8_8_8:
        case xenos::TextureFormat::k_8_8_8_8_A: out = { VK_FORMAT_R8G8B8A8_UNORM, kIdentitySwizzle }; return true;
        case xenos::TextureFormat::k_2_10_10_10: out = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, kIdentitySwizzle }; return true;
        case xenos::TextureFormat::k_16: out = { VK_FORMAT_R16_UNORM, PackSwizzle(R, R, R, R) }; return true;
        case xenos::TextureFormat::k_16_16: out = { VK_FORMAT_R16G16_UNORM, PackSwizzle(R, G, G, G) }; return true;
        case xenos::TextureFormat::k_16_16_16_16_FLOAT: out = { VK_FORMAT_R16G16B16A16_SFLOAT, kIdentitySwizzle }; return true;
        case xenos::TextureFormat::k_32_FLOAT: out = { VK_FORMAT_R32_SFLOAT, PackSwizzle(R, R, R, R) }; return true;
        case xenos::TextureFormat::k_DXT1: out = { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, kIdentitySwizzle }; return true;
        case xenos::TextureFormat::k_DXT2_3: out = { VK_FORMAT_BC2_UNORM_BLOCK, kIdentitySwizzle }; return true;
        case xenos::TextureFormat::k_DXT4_5: out = { VK_FORMAT_BC3_UNORM_BLOCK, kIdentitySwizzle }; return true;
        default: (void)ONE; return false;
        }
    }

    // Guest swizzle (3 bits per component: 0-3 = XYZW, 4 = 0, 5 = 1) over the host swizzle.
    uint32_t CombineSwizzle(uint32_t guestSwizzle, uint32_t hostSwizzle)
    {
        uint32_t out = 0;
        for (uint32_t c = 0; c < 4; ++c)
        {
            const uint32_t g = (guestSwizzle >> (3 * c)) & 7;
            uint32_t v;
            if (g < 4) v = (hostSwizzle >> (3 * g)) & 7;
            else if (g == 4) v = VK_COMPONENT_SWIZZLE_ZERO;
            else v = VK_COMPONENT_SWIZZLE_ONE;
            out |= v << (3 * c);
        }
        return out;
    }

    struct Texture
    {
        Image img;
        uint64_t uploadedFrame = ~0ull;
        uint64_t lastUsedFrame = 0; // for eviction (EvictUnusedImages)
        uint64_t contentHash = 0; // XXH3 of the guest bytes last uploaded
        // Write tracking (as vertex pages): unchanged guest bytes are not even hashed. Textures the
        // CPU rewrites frame after frame (movies) fall back to hashing (no page faults).
        uint32_t watchEpoch = 0;
        bool watched = false;
        uint32_t changedStreak = 0;
    };
    std::map<std::vector<uint32_t>, std::unique_ptr<Texture>> g_textures;
    Image g_null2D, g_null3D, g_nullCube;

    void SwapDwords(uint8_t* data, size_t bytes, xenos::Endian endian)
    {
        if (endian == xenos::Endian::kNone) return;
        for (size_t i = 0; i + 4 <= bytes; i += 4)
        {
            uint32_t v;
            std::memcpy(&v, data + i, 4);
            switch (endian)
            {
            case xenos::Endian::k8in16: v = ((v >> 8) & 0x00FF00FF) | ((v << 8) & 0xFF00FF00); break;
            case xenos::Endian::k8in32: v = __builtin_bswap32(v); break;
            case xenos::Endian::k16in32: v = (v >> 16) | (v << 16); break;
            default: break;
            }
            std::memcpy(data + i, &v, 4);
        }
    }

    // Uploads the base level of a 2D texture from guest memory (records the copy).
    Texture* GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch, const HostFormat& host)
    {
        const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
        const uint32_t base = fetch.base_address << 12;
        std::vector<uint32_t> key = { base, uint32_t(fetch.format), width, height, fetch.tiled, fetch.pitch, uint32_t(fetch.endianness) };
        auto& slot = g_textures[key];
        if (!slot)
        {
            if (width == 512 && height == 256 && !fetch.tiled && fetch.format == xenos::TextureFormat::k_8_8_8_8 && base >= 0x18000000u)
                std::printf("[render] new 512x256 kit texture 0x%08X (frame %llu)\n", base, (unsigned long long)g_frame);
            slot = std::make_unique<Texture>();
            if (!CreateImage(slot->img, host.format, width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            {
                WarnOnce("texture creation failed");
                slot.reset();
                return nullptr;
            }
        }
        Texture* tex = slot.get();
        // PES_WATCH_KIT_BUILD=1 (debug): at the first baked 512x256 kit, report who writes the
        // memory after it (where the next player's kit gets built).
        static const bool watchKitBuild = std::getenv("PES_WATCH_KIT_BUILD") != nullptr;
        static bool kitWatchArmed = false;
        if (watchKitBuild && !kitWatchArmed && base >= 0x18000000u && width == 512 && height == 256 && !fetch.tiled &&
            fetch.format == xenos::TextureFormat::k_8_8_8_8)
        {
            kitWatchArmed = true;
            std::printf("[render] first baked kit texture at 0x%08X; watching the next 8 MB\n", base);
            MemoryWatch::ReportNextWrite(base + 0x80000, 0x800000);
        }
        tex->lastUsedFrame = g_frame;
        if (tex->uploadedFrame == g_frame) return tex;

        const FormatInfo* info = FormatInfo::Get(fetch.format);
        const auto layout = texture_util::GetGuestTextureLayout(xenos::DataDimension::k2DOrStacked, fetch.pitch, width, height, 1,
            fetch.tiled, fetch.format, fetch.packed_mips, true, 0);
        const uint32_t blockW = info->block_width, blockH = info->block_height, bpb = info->bytes_per_block();
        const uint32_t blocksX = (width + blockW - 1) / blockW, blocksY = (height + blockH - 1) / blockH;
        const uint32_t srcBytes = layout.base.level_data_extent_bytes;
        if (!srcBytes || base + srcBytes > kSharedMemorySize) return tex;

        // PES_TEX_HASH=1: hash every frame (no write tracking), for A/B.
        static const bool texHashOnly = std::getenv("PES_TEX_HASH") != nullptr;
        if (!texHashOnly && tex->watched && tex->uploadedFrame != ~0ull && !MemoryWatch::WrittenSince(base, srcBytes, tex->watchEpoch))
        {
            tex->uploadedFrame = g_frame;
            return tex;
        }
        // Epoch, then protect, then read the bytes (a write in between is recorded as newer).
        const uint32_t epoch = MemoryWatch::Epoch();
        const bool watch = !texHashOnly && tex->changedStreak < 3;
        if (watch) MemoryWatch::Protect(base, srcBytes);
        tex->watched = watch;
        tex->watchEpoch = epoch;
        // Skip the upload if the guest bytes did not change since the last one.
        const auto hashStart = std::chrono::steady_clock::now();
        const uint64_t hash = XXH3_64bits(g_xeMemory.TranslatePhysical(base), srcBytes);
        g_texHashUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - hashStart).count();
        g_texHashBytes += srcBytes;
        if (tex->uploadedFrame != ~0ull && hash == tex->contentHash)
        {
            tex->uploadedFrame = g_frame;
            tex->changedStreak = 0;
            return tex;
        }
        if (tex->uploadedFrame != ~0ull) ++tex->changedStreak;
        tex->contentHash = hash;
        ++g_time.textureUploads;

        std::vector<uint8_t> src(srcBytes);
        std::memcpy(src.data(), g_xeMemory.TranslatePhysical(base), srcBytes);
        SwapDwords(src.data(), src.size(), fetch.endianness);

        const bool decode = g_bcDecode && IsBC(fetch.format);
        static std::vector<uint8_t> blocks; // decode path: linear blocks, then RGBA into the pool
        VkDeviceSize offset = 0;
        VkBuffer staging = VK_NULL_HANDLE;
        uint8_t* dst = nullptr;
        if (decode)
        {
            blocks.assign(size_t(blocksX) * blocksY * bpb, 0);
            dst = blocks.data();
        }
        else if (!(dst = StagingAllocate(VkDeviceSize(blocksX) * blocksY * bpb, staging, offset)))
            return tex;
        uint32_t bpbLog2 = 0;
        while ((1u << bpbLog2) < bpb) ++bpbLog2;
        const uint32_t pitchBlocks = layout.base.row_pitch_bytes / bpb; // tiled: aligned to 32 blocks
        for (uint32_t by = 0; by < blocksY; ++by)
            for (uint32_t bx = 0; bx < blocksX; ++bx)
            {
                const uint32_t s = fetch.tiled ? uint32_t(texture_address::Tiled2D(int32_t(bx), int32_t(by), pitchBlocks, bpbLog2))
                                               : by * layout.base.row_pitch_bytes + bx * bpb;
                if (s + bpb <= srcBytes)
                    std::memcpy(dst + (size_t(by) * blocksX + bx) * bpb, src.data() + s, bpb);
            }

        VkBufferImageCopy copy{};
        if (decode)
        {
            // Blocks -> RGBA8 rows of blocksX * 4 pixels; the copy takes the image's width x height of them.
            const uint32_t rowPixels = blocksX * 4;
            uint8_t* rgba = StagingAllocate(VkDeviceSize(rowPixels) * blocksY * 4 * 4, staging, offset);
            if (!rgba) return tex;
            uint8_t px[16 * 4];
            for (uint32_t by = 0; by < blocksY; ++by)
                for (uint32_t bx = 0; bx < blocksX; ++bx)
                {
                    DecodeBCBlock(fetch.format, blocks.data() + (size_t(by) * blocksX + bx) * bpb, px);
                    for (uint32_t y = 0; y < 4; ++y)
                        std::memcpy(rgba + ((size_t(by) * 4 + y) * rowPixels + bx * 4) * 4, px + y * 16, 16);
                }
            copy.bufferRowLength = rowPixels;
            copy.bufferImageHeight = blocksY * 4;
        }
        Transition(tex->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        copy.bufferOffset = offset;
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { blocksX * blockW, blocksY * blockH, 1 };
        copy.imageExtent.width = std::min(copy.imageExtent.width, width) == width ? width : copy.imageExtent.width;
        copy.imageExtent.height = std::min(copy.imageExtent.height, height) == height ? height : copy.imageExtent.height;
        MarkGpu(3);
        vkCmdCopyBufferToImage(g_cmd, staging, tex->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        MarkGpu(2);
        tex->uploadedFrame = g_frame;
        return tex;
    }

    // ------------------------------------------------------------------ composed textures
    // Our resolves stay on the GPU as images in their own shape, but the game addresses memory:
    // PES keeps player names (256x64), numbers (64x64) and kit atlases (128x128) in a reused
    // pool, and resolves a kit into a larger pool surface at an offset - over older name/number
    // surfaces that start exactly at the kit's address (seen in surface dumps, 2026-11-07).
    // A fetch whose memory is covered by surfaces of another shape/offset is therefore rebuilt
    // from all of them, newest last, following the fetch's layout:
    // - tiled fetch: in the Xenos tiled layout every 32x32 texel tile is one contiguous 4 KB
    //   block (at 32 bpp) whose inner arrangement depends neither on the pitch nor on the tile's
    //   position (texture_address::Tiled2D), so re-reading with another pitch is a permutation
    //   of tiles: tile (x, y) of a surface lies at base + (y * pitch/32 + x) * 4 KB.
    // - linear fetch (what PES uses for kits): memory is rows of `pitch` texels; a surface row y
    //   lies at base + y * pitch * 4, and is re-wrapped into the fetch's rows.
    struct ComposedTexture
    {
        Image img;
        uint64_t signature = 0; // surfaces + their generations it was built from
        uint64_t lastUsedFrame = 0;
        bool rbSwapped = false;
    };
    std::map<std::array<uint32_t, 5>, std::unique_ptr<ComposedTexture>> g_composed; // base, width, height, pitch, tiled
    uint64_t g_composedBuilds = 0, g_composedUses = 0;

    // Frees cached images nothing has used for a while. Before 2026-11-09 no image was ever freed:
    // the texture cache (keyed by address/format/size), composed textures, surface crops and
    // retired resolve surfaces grew for the whole session - fatal on iPad (the OS kills the app).
    // Runs in BeginFrame after the slot's fence wait. An image last used kEvictFrames ago (or a
    // surface retired at an earlier sweep, >= 60 frames ago) is no longer referenced by any
    // command buffer in flight (2 frames) or by a descriptor set (set caches are per frame).
    // PES_NO_EVICT=1 disables it (A/B).
    void EvictUnusedImages()
    {
        // Images replaced by render-target growth: free once no frame in flight can use them.
        for (auto it = g_retired.begin(); it != g_retired.end();)
            if (g_frame - it->frame > kFramesInFlight)
            {
                for (VkFramebuffer fb : it->framebuffers) vkDestroyFramebuffer(g_dev, fb, nullptr);
                for (VkImageView v : it->views) vkDestroyImageView(g_dev, v, nullptr);
                if (it->buffer.buffer) DestroyBuffer(it->buffer);
                DestroyImage(it->img);
                it = g_retired.erase(it);
            }
            else ++it;

        constexpr uint64_t kSweepEvery = 60, kEvictFrames = 600;
        static const bool disabled = std::getenv("PES_NO_EVICT") != nullptr;
        static uint64_t lastSweep = 0;
        if (disabled || g_frame < kEvictFrames || g_frame - lastSweep < kSweepEvery)
            return;
        lastSweep = g_frame;
        auto stale = [](uint64_t lastUsed) { return g_frame - lastUsed > kEvictFrames; };

        for (auto it = g_textures.begin(); it != g_textures.end();)
            if (stale(it->second->lastUsedFrame)) { DestroyImage(it->second->img); it = g_textures.erase(it); }
            else ++it;
        for (auto it = g_composed.begin(); it != g_composed.end();)
            if (stale(it->second->lastUsedFrame)) { DestroyImage(it->second->img); it = g_composed.erase(it); }
            else ++it;
        // Live surfaces nothing resolved into or sampled for kEvictFrames are retired (freed at the next
        // sweep below) when guest memory needs nothing from them: it already holds their content
        // (written back), or the CPU has rewritten that memory since the resolve (stale). Surfaces
        // whose content exists only on the GPU are kept (a write-back would be needed first).
        for (auto& sp : g_surfaces)
        {
            Surface& s = *sp;
            if (s.base == 0xFFFFFFFFu || !stale(std::max(s.lastResolveFrame, s.lastSampledFrame))) continue;
            if (s.writtenBackGen == s.generation || !SurfaceStillInMemory(&s))
            {
                s.base = 0xFFFFFFFFu;
                ++g_surfaceEpoch;
                ++g_surfacesEvicted;
            }
        }
        bool removed = false;
        for (auto it = g_surfaces.begin(); it != g_surfaces.end();)
        {
            Surface& s = **it;
            if (s.base == 0xFFFFFFFFu)
            {
                if (s.retiredFrame == 0) s.retiredFrame = g_frame; // destroyed at the next sweep
                else
                {
                    for (auto& [key, crop] : s.crops) DestroyImage(crop.img);
                    DestroyImage(s.img);
                    it = g_surfaces.erase(it);
                    removed = true;
                    continue;
                }
            }
            for (auto c = s.crops.begin(); c != s.crops.end();)
                if (stale(c->second.lastUsedFrame)) { DestroyImage(c->second.img); c = s.crops.erase(c); }
                else ++c;
            ++it;
        }
        if (removed) ++g_surfaceEpoch; // rebuilds the sorted surface index

        // Render targets: one per (EDRAM base, format, pitch, MSAA) ever used, each sized for all of
        // EDRAM (~10 MB, depth ~20 MB). Free those unused for kEvictFrames that own no EDRAM tile (no
        // content anyone could still copy from them), with their framebuffers and views.
        std::unordered_map<const RenderTarget*, uint32_t> owned;
        for (const RenderTarget* o : g_tileOwner) if (o) ++owned[o];
        for (auto it = g_rts.begin(); it != g_rts.end();)
        {
            RenderTarget* rt = it->second.get();
            if (!stale(rt->lastUsedFrame) || owned.count(rt) || rt == g_passColor || rt == g_passDepth) { ++it; continue; }
            for (auto fb = g_framebuffers.begin(); fb != g_framebuffers.end();)
                if (fb->first.first == rt || fb->first.second == rt) { vkDestroyFramebuffer(g_dev, fb->second, nullptr); fb = g_framebuffers.erase(fb); }
                else ++fb;
            if (rt->depthView) vkDestroyImageView(g_dev, rt->depthView, nullptr);
            if (rt->stencilView) vkDestroyImageView(g_dev, rt->stencilView, nullptr);
            DestroyImage(rt->img);
            ++g_rtsEvicted;
            it = g_rts.erase(it);
        }
    }

    // memoryExact: set when guest memory already holds every overlapping resolve (mirrored):
    // the fetch is then read from memory with its own layout, exactly as on the console.
    ComposedTexture* ComposeFromSurfaces(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t base, bool& memoryExact)
    {
        static const bool disabled = std::getenv("PES_NO_COMPOSE") != nullptr; // A/B: old behaviour
        if (disabled) return nullptr;
        const FormatInfo* fi = FormatInfo::Get(fetch.format);
        if (!fi || fi->block_width != 1 || fi->block_height != 1 || fi->bits_per_pixel != 32) return nullptr;
        if (fetch.format == xenos::TextureFormat::k_24_8 || fetch.format == xenos::TextureFormat::k_24_8_FLOAT) return nullptr;
        const bool tiled = fetch.tiled;
        const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
        const uint32_t pitch = std::max<uint32_t>(fetch.pitch, 1) * 32; // texels per row (pitch field is texels >> 5)
        constexpr uint32_t kTileBytes = 32 * 32 * 4;
        const uint64_t bytes = tiled ? uint64_t(pitch / 32) * ((height + 31) / 32) * kTileBytes : uint64_t(pitch) * height * 4;
        const uint64_t rangeEnd = uint64_t(base) + bytes;

        // Live surfaces sorted by base (rebuilt when one is created or retired): only those that
        // can reach [base, rangeEnd) are checked, not every surface for every fetch.
        static std::vector<Surface*> index;
        static uint64_t indexEpoch = ~0ull;
        static uint32_t maxSurfaceSize = 0;
        if (indexEpoch != g_surfaceEpoch)
        {
            indexEpoch = g_surfaceEpoch;
            index.clear();
            maxSurfaceSize = 0;
            for (auto& sp : g_surfaces)
                if (sp->base != 0xFFFFFFFFu)
                {
                    index.push_back(sp.get());
                    maxSurfaceSize = std::max(maxSurfaceSize, sp->size);
                }
            std::sort(index.begin(), index.end(), [](const Surface* a, const Surface* b) { return a->base < b->base; });
        }
        const uint32_t searchFrom = base > maxSurfaceSize ? base - maxSurfaceSize : 0;
        auto first = std::lower_bound(index.begin(), index.end(), searchFrom, [](const Surface* s, uint32_t a) { return s->base < a; });

        // Surfaces covering the fetch's memory (same texel size and host format).
        static std::vector<Surface*> parts;
        parts.clear();
        for (auto it = first; it != index.end() && uint64_t((*it)->base) < rangeEnd; ++it)
        {
            Surface* s = *it;
            if (s->base == 0xFFFFFFFFu || s->isDepth || s->bytesPerPixel != 4) continue;
            if (uint64_t(s->base) >= rangeEnd || uint64_t(s->base) + s->size <= base) continue;
            if (tiled && (s->pitch % 32 || (int64_t(s->base) - int64_t(base)) % kTileBytes)) continue;
            if (!tiled && (int64_t(s->base) - int64_t(base)) % 4) continue;
            if (!parts.empty() && s->img.format != parts[0]->img.format) continue;
            parts.push_back(s);
        }
        if (parts.empty()) return nullptr;
        // Memory semantics: a later resolve overwrites what earlier ones left in the same bytes
        // (PES reuses a pool for names, numbers and kits; a kit is resolved into a larger pool
        // surface at an offset, over an older name/number surface that starts at the kit's
        // address). Copy oldest first so the newest content ends up on top.
        std::sort(parts.begin(), parts.end(), [](const Surface* a, const Surface* b) { return a->generation < b->generation; });
        // The newest surface starts here with the fetch's shape: it is the texture itself.
        const Surface* newest = parts.back();
        if (newest->base == base && newest->pitch == pitch && newest->height >= height &&
            (parts.size() == 1 || uint64_t(newest->size) >= bytes)) return nullptr;
        if (std::all_of(parts.begin(), parts.end(), [](const Surface* s) { return s->writtenBackGen == s->generation; }))
        {
            memoryExact = true;
            return nullptr;
        }
        for (Surface* s : parts)
            if (!SurfaceStillInMemory(s)) return nullptr; // the CPU rewrote this memory: read it from guest memory

        uint64_t words[2 * 64];
        uint32_t n = 0;
        for (Surface* s : parts) s->lastSampledFrame = g_frame;
        for (Surface* s : parts)
            if (n + 2 <= std::size(words)) { words[n++] = uint64_t(reinterpret_cast<uintptr_t>(s)); words[n++] = s->generation; }
        const uint64_t signature = XXH3_64bits(words, n * 8);

        auto& slot = g_composed[{ base, width, height, pitch, uint32_t(tiled) }];
        if (!slot)
        {
            slot = std::make_unique<ComposedTexture>();
            if (!CreateImage(slot->img, parts[0]->img.format, width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            {
                WarnOnce("composed texture creation failed");
                slot.reset();
                return nullptr;
            }
        }
        ComposedTexture* ct = slot.get();
        ct->lastUsedFrame = g_frame;
        ct->rbSwapped = parts[0]->rbSwapped;
        ++g_composedUses;
        if (ct->signature == signature) return ct;
        ct->signature = signature;
        ++g_composedBuilds;
        {
            static uint32_t logs = 0;
            if (logs++ < 60)
            {
                std::printf("[render] composed %s texture 0x%08X %ux%u (pitch %u) from", tiled ? "tiled" : "linear", base, width, height, pitch);
                for (Surface* s : parts) std::printf(" [0x%08X %ux%u]", s->base, s->pitch, s->height);
                std::printf("\n");
            }
        }

        EndPass();
        ClearImage(ct->img, VkClearColorValue{});
        VkMemoryBarrier mb{ VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
        static std::vector<VkImageCopy> regions;
        auto addRegion = [&](uint32_t sx, uint32_t sy, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h) {
            VkImageCopy c{};
            c.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            c.srcOffset = { int32_t(sx), int32_t(sy), 0 };
            c.dstOffset = { int32_t(dx), int32_t(dy), 0 };
            c.extent = { w, h, 1 };
            regions.push_back(c);
        };
        for (Surface* s : parts)
        {
            regions.clear();
            const uint32_t sw = std::min(s->pitch, s->img.width), sh = std::min(s->height, s->img.height);
            if (tiled)
            {
                const uint32_t sTilesX = s->pitch / 32, sTilesY = (s->height + 31) / 32, fTilesX = pitch / 32;
                const int64_t fTiles = int64_t(fTilesX) * ((height + 31) / 32);
                for (uint32_t ty = 0; ty < sTilesY; ++ty)
                    for (uint32_t tx = 0; tx < sTilesX; ++tx)
                    {
                        const int64_t j = (int64_t(s->base) + int64_t(ty * sTilesX + tx) * kTileBytes - int64_t(base)) / kTileBytes;
                        if (j < 0 || j >= fTiles) continue;
                        const uint32_t fx = uint32_t(j % fTilesX) * 32, fy = uint32_t(j / fTilesX) * 32;
                        const uint32_t sx = tx * 32, sy = ty * 32;
                        if (sx >= sw || sy >= sh || fx >= width || fy >= height) continue;
                        addRegion(sx, sy, fx, fy, std::min({ 32u, sw - sx, width - fx }), std::min({ 32u, sh - sy, height - fy }));
                    }
            }
            else
            {
                // Each surface row is a run of texels in memory; split it at the fetch's row ends.
                const int64_t first = (int64_t(s->base) - int64_t(base)) / 4; // texel index of the surface's first texel
                for (uint32_t y = 0; y < sh; ++y)
                {
                    uint32_t x = 0;
                    while (x < sw)
                    {
                        const int64_t idx = first + int64_t(y) * s->pitch + x;
                        if (idx < 0) { x += uint32_t(std::min<int64_t>(-idx, sw - x)); continue; }
                        const uint32_t v = uint32_t(idx / pitch), u = uint32_t(idx % pitch);
                        const uint32_t run = std::min(sw - x, pitch - u);
                        if (v >= height) break;
                        if (u < width) addRegion(x, y, u, v, std::min(run, width - u), 1);
                        x += run;
                    }
                }
            }
            if (regions.empty()) continue;
            Transition(s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            vkCmdCopyImage(g_cmd, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ct->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                uint32_t(regions.size()), regions.data());
        }
        return ct;
    }

    // Image view for a texture fetch constant: a resolved surface at that address, or the
    // texture uploaded from guest memory, or a null image.
    VkImageView TextureView(uint32_t fetchIndex, xenos::FetchOpDimension dimension)
    {
        const auto fetch = g_regs.GetTextureFetch(fetchIndex);
        Image* nullImage = dimension == xenos::FetchOpDimension::k3DOrStacked ? &g_null3D
            : dimension == xenos::FetchOpDimension::kCube ? &g_nullCube : &g_null2D;
        auto useNull = [&]() { Transition(*nullImage, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); return SampledView(*nullImage, kIdentitySwizzle); };
        if (fetch.type != xenos::FetchConstantType::kTexture || dimension == xenos::FetchOpDimension::k3DOrStacked ||
            dimension == xenos::FetchOpDimension::kCube || fetch.dimension != xenos::DataDimension::k2DOrStacked)
            return useNull();

        HostFormat host;
        const uint32_t base = fetch.base_address << 12;
        // Resolves of another shape covering this texture's memory: read from guest memory when
        // they were mirrored there, else rebuilt from the surfaces.
        bool memoryExact = false;
        if (ComposedTexture* ct = ComposeFromSurfaces(fetch, base, memoryExact))
        {
            Transition(ct->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return SampledView(ct->img, CombineSwizzle(fetch.swizzle, ct->rbSwapped ? kSwapRBSwizzle : kIdentitySwizzle));
        }
        if (Surface* s = memoryExact ? nullptr : FindSurfaceForTexture(base, fetch))
        {
            s->lastSampledFrame = g_frame;
            // A resolve with copy_dest_swap exchanged red and blue in memory: guest channel 0 is the image's blue.
            const uint32_t swz = CombineSwizzle(fetch.swizzle, s->rbSwapped ? kSwapRBSwizzle : kIdentitySwizzle);
            // Surfaces are sized to the tiled layout (720 -> 736 rows, 360 -> 384): a smaller fetch
            // must not see the padding, or normalized coordinates stretch the image (PES' blur
            // chain lost rows at every pass -> black bars in close-ups, 2026-11-08). Sample a copy
            // of exactly the fetch's size, refreshed after every resolve into the surface.
            // PES_NO_SURFACE_CROP=1: old behaviour (A/B).
            static const bool noCrop = std::getenv("PES_NO_SURFACE_CROP") != nullptr;
            const uint32_t fw = uint32_t(fetch.size_2d.width) + 1, fh = uint32_t(fetch.size_2d.height) + 1;
            if (!noCrop && fw <= s->img.width && fh <= s->img.height && (fw < s->img.width || fh < s->img.height))
            {
                Surface::Crop& c = s->crops[(fw << 16) | fh];
                if (!c.img.image && !CreateImage(c.img, s->img.format, fw, fh, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
                    c.img = {};
                if (c.img.image)
                {
                    c.lastUsedFrame = g_frame;
                    if (c.generation != s->generation)
                    {
                        Transition(s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                        Transition(c.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                        VkImageCopy copy{};
                        copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                        copy.extent = { fw, fh, 1 };
                        vkCmdCopyImage(g_cmd, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                        c.generation = s->generation;
                    }
                    Transition(c.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    return SampledView(c.img, swz);
                }
            }
            Transition(s->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return SampledView(s->img, swz);
        }
        if (!GetHostFormat(fetch.format, host))
        {
            WarnOnce("unsupported texture format " + std::to_string(uint32_t(fetch.format)));
            return useNull();
        }
        Texture* tex = GetTexture(fetch, host);
        if (!tex) return useNull();
        Transition(tex->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return SampledView(tex->img, CombineSwizzle(fetch.swizzle, host.swizzle));
    }

    // ------------------------------------------------------------------ samplers
    std::map<uint64_t, VkSampler> g_samplers;

    VkSamplerAddressMode AddressMode(xenos::ClampMode m)
    {
        switch (m)
        {
        case xenos::ClampMode::kRepeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case xenos::ClampMode::kMirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case xenos::ClampMode::kClampToBorder:
        case xenos::ClampMode::kMirrorClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        case xenos::ClampMode::kMirrorClampToEdge:
        case xenos::ClampMode::kMirrorClampToHalfway: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        }
    }

    VkSampler GetSampler(const SpirvShader::SamplerBinding& binding)
    {
        const auto fetch = g_regs.GetTextureFetch(binding.fetch_constant);
        auto pick = [](xenos::TextureFilter b, xenos::TextureFilter f) { return b == xenos::TextureFilter::kUseFetchConst ? f : b; };
        const auto mag = pick(binding.mag_filter, fetch.mag_filter);
        const auto min = pick(binding.min_filter, fetch.min_filter);
        // As Xenia: clamp modes only for the texture's dimensions (a 1D texture is a 1-texel-high
        // image here; its Y mode must not bring in a border), border colour from the fetch constant.
        // PES' depth-of-field weight is a 256x1 lookup table: with the fetch's Y clamp-to-border,
        // linear filtering blended the border in (black bars in pre-match close-ups, 2026-11-08).
        xenos::ClampMode cx, cy, cz;
        texture_util::GetClampModesForDimension(fetch, cx, cy, cz);
        const bool usesBorder = xenos::ClampModeUsesBorder(cx) || xenos::ClampModeUsesBorder(cy) || xenos::ClampModeUsesBorder(cz);
        const uint32_t border = usesBorder ? uint32_t(fetch.border_color) : 0u;
        const uint64_t key = uint64_t(mag) | (uint64_t(min) << 2) | (uint64_t(cx) << 4) | (uint64_t(cy) << 7) |
            (uint64_t(cz) << 10) | (uint64_t(border) << 13);
        auto it = g_samplers.find(key);
        if (it != g_samplers.end()) return it->second;
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = mag == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.minFilter = min == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = AddressMode(cx);
        si.addressModeV = AddressMode(cy);
        si.addressModeW = AddressMode(cz);
        si.maxLod = 0.25f;
        si.borderColor = border == uint32_t(xenos::BorderColor::k_ABGR_White) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                                               : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkSampler s = VK_NULL_HANDLE;
        vkCreateSampler(g_dev, &si, nullptr, &s);
        g_samplers.emplace(key, s);
        return s;
    }

    // ------------------------------------------------------------------ shaders
    struct ShaderEntry
    {
        std::unique_ptr<SpirvShader> shader;
        std::unordered_map<uint64_t, VkShaderModule> modules; // by modification
    };
    std::unordered_map<uint64_t, ShaderEntry> g_shaderCache;
    ShaderEntry* g_current[2] = {};

    VkShaderModule GetModule(ShaderEntry& e, uint64_t modification)
    {
        auto it = e.modules.find(modification);
        if (it != e.modules.end()) return it->second;
        ScopedTimer timer{ g_time.modules };
        ++g_time.newModules;
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

    // ------------------------------------------------------------------ layouts + pipelines
    VkDescriptorSetLayout g_layoutShared = VK_NULL_HANDLE, g_layoutConstants = VK_NULL_HANDLE, g_layoutEmpty = VK_NULL_HANDLE;
    VkDescriptorPool g_persistentPool = VK_NULL_HANDLE;
    VkDescriptorSet g_sharedSet = VK_NULL_HANDLE;
    std::map<std::pair<uint32_t, uint32_t>, VkDescriptorSetLayout> g_textureLayouts[2]; // [vertex, pixel] by (textures, samplers)

    struct PipelineLayout
    {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout textures[2] = {};
    };
    std::map<std::vector<uint32_t>, PipelineLayout> g_pipelineLayouts;
    std::map<std::vector<uint64_t>, VkPipeline> g_pipelines;
    // Driver pipeline cache, persisted in userdata/vulkan_pipeline_cache.bin so the (slow)
    // compilation of translated shaders happens once, not on every launch.
    VkPipelineCache g_pipelineCache = VK_NULL_HANDLE;
    bool g_pipelineCacheDirty = false;
    const char* kPipelineCachePath = "userdata/vulkan_pipeline_cache.bin";

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

    struct PipelineDesc
    {
        VkShaderModule vs, ps;
        VkPrimitiveTopology topology;
        bool primitiveRestart;
        VkCullModeFlags cull;
        VkFrontFace frontFace;
        uint32_t colorMask;   // RT0, 4 bits
        uint32_t blend;       // RB_BLENDCONTROL0 & 0x1FFF1FFF
        VkFormat format;      // color target (always present for now)
        VkFormat depthFormat; // VK_FORMAT_UNDEFINED = no depth/stencil attachment
        uint32_t depthControl; // normalized RB_DEPTHCONTROL (draw_util::GetNormalizedDepthControl)
        bool depthClamp;      // PA_CL_CLIP_CNTL.clip_disable: the Xenos clamps z instead of clipping (D3D clears write z slightly below 0)
        PipelineLayout* layout;
    };

    VkPipeline GetPipeline(const PipelineDesc& d)
    {
        const std::vector<uint64_t> key = { uint64_t(d.vs), uint64_t(d.ps), uint64_t(d.topology), d.primitiveRestart, d.cull,
            uint64_t(d.frontFace), d.colorMask, d.blend, uint64_t(d.format), uint64_t(d.layout->layout), uint64_t(d.depthFormat),
            d.depthControl, d.depthClamp };
        auto it = g_pipelines.find(key);
        if (it != g_pipelines.end()) return it->second;
        ScopedTimer timer{ g_time.pipelines };
        ++g_time.newPipelines;

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

    // ------------------------------------------------------------------ per-draw capture
    // PES_CAPTURE=<seconds>: records the first whole frame after that time. After every draw and
    // resolve, a thumbnail of the color render target goes into an atlas (12 x 45 slots of
    // 160x90, the target's top-left 1280x720 pixels scaled down) and a line into a step log:
    // userdata/capture/atlas.ppm + steps.txt (slot numbers are row-major in the atlas).
    constexpr uint32_t kThumbW = 160, kThumbH = 90, kThumbCols = 12, kThumbRows = 45;
    Image g_capAtlas;
    enum class CaptureState { Off, Armed, Active, Done } g_capState = CaptureState::Off;
    FILE* g_capLog = nullptr;
    std::string g_capDir = "userdata/capture";
    std::deque<std::string> g_controlCapture; // PES_CONTROL "capture <name>" queue: consecutive frames, one each
    std::string g_controlCaptureAck;   // name of the running control capture (acknowledged when written)
    bool g_capArmed = false, g_capArmHit = false; // "capture-on-resolve": keep only a frame with a matching resolve
    uint32_t g_capArmMin = 0, g_capArmHitAddr = 0, g_capArmW = 128, g_capArmH = 128;
    bool g_controlDumpSurfaces = false; // PES_CONTROL "surfaces": dump small surfaces + composed textures
    bool g_controlDumpAllSurfaces = false; // "surfaces all": every RGBA8 surface, any size
    void CaptureNote(const char* fmt, ...)
    {
        if (g_capState != CaptureState::Active || !g_capLog) return;
        va_list args;
        va_start(args, fmt);
        std::vfprintf(g_capLog, fmt, args);
        va_end(args);
        std::fputc('\n', g_capLog);
    }
    uint32_t g_capStep = 0, g_capSlots = 0;

    struct CaptureProbe { uint32_t step; bool pre = false; int32_t color = -1, depth = -1, stencil = -1; VkFormat colorFormat = VK_FORMAT_UNDEFINED; };
    std::vector<CaptureProbe> g_capProbes;
    Buffer g_capProbeBuf;
    uint32_t g_capProbeUsed = 0;
    RenderTarget* g_capDepthRt = nullptr; // depth target of the current draw (probe)

    // Probe texels (see PES_CAPTURE_PROBE); pre = before the step's draw.
    void CaptureProbeNow(RenderTarget* color, RenderTarget* depth, bool pre)
    {
        if (g_capState != CaptureState::Active) return;
        // PES_CAPTURE_PROBE="x,y": read back that texel of the color target and of the depth target
        // (g_capDepthRt) after every step -> userdata/capture/probes.txt.
        static const std::array<int32_t, 2> probe = [] {
            std::array<int32_t, 2> p{ -1, -1 };
            if (const char* e = std::getenv("PES_CAPTURE_PROBE")) std::sscanf(e, "%d,%d", &p[0], &p[1]);
            return p;
        }();
        if (probe[0] >= 0 && (g_capProbeBuf.buffer || CreateBuffer(g_capProbeBuf, 1u << 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT)))
        {
            CaptureProbe pr{ g_capStep };
            auto copyTexel = [&](RenderTarget* t, VkImageAspectFlags aspect, int32_t& outOffset) {
                if (!t || uint32_t(probe[0]) >= t->img.width || uint32_t(probe[1]) >= t->img.height || g_capProbeUsed + 16 > g_capProbeBuf.size) return;
                Transition(t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy c{};
                c.bufferOffset = g_capProbeUsed;
                c.imageSubresource = { aspect, 0, 0, 1 };
                c.imageOffset = { probe[0], probe[1], 0 };
                c.imageExtent = { 1, 1, 1 };
                vkCmdCopyImageToBuffer(g_cmd, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_capProbeBuf.buffer, 1, &c);
                outOffset = int32_t(g_capProbeUsed);
                g_capProbeUsed += 16;
            };
            pr.pre = pre;
            if (color) { copyTexel(color, VK_IMAGE_ASPECT_COLOR_BIT, pr.color); pr.colorFormat = color->format; }
            RenderTarget* d = depth;
            copyTexel(d, VK_IMAGE_ASPECT_DEPTH_BIT, pr.depth);
            copyTexel(d, VK_IMAGE_ASPECT_STENCIL_BIT, pr.stencil);
            if (pr.color >= 0 || pr.depth >= 0) g_capProbes.push_back(pr);
        }
    }

    Image* g_capThumbImage = nullptr; // thumbnail this image instead of the step target (resolve destination)
    void CaptureStep(RenderTarget* rt, const char* fmt, ...)
    {
        if (g_capState != CaptureState::Active || !g_capLog) return;
        char text[2048];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(text, sizeof(text), fmt, args);
        va_end(args);
        int slot = -1;
        Image* thumbImg = g_capThumbImage ? g_capThumbImage : rt ? &rt->img : nullptr;
        g_capThumbImage = nullptr;
        if (rt && !rt->isDepth && thumbImg && g_capSlots < kThumbCols * kThumbRows)
        {
            slot = int(g_capSlots++);
            Transition(*thumbImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Transition(g_capAtlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit b{};
            b.srcSubresource = b.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            // PES_CAPTURE_RECT="x,y,w,h": thumbnail that region of every target instead (zoom on a detail).
            static const std::array<int32_t, 4> zoom = [] {
                std::array<int32_t, 4> z{ 0, 0, 0, 0 };
                if (const char* e = std::getenv("PES_CAPTURE_RECT"))
                    std::sscanf(e, "%d,%d,%d,%d", &z[0], &z[1], &z[2], &z[3]);
                return z;
            }();
            if (zoom[2] > 0 && zoom[3] > 0)
            {
                b.srcOffsets[0] = { std::min<int32_t>(zoom[0], int32_t(thumbImg->width) - 1), std::min<int32_t>(zoom[1], int32_t(thumbImg->height) - 1), 0 };
                b.srcOffsets[1] = { std::min<int32_t>(zoom[0] + zoom[2], int32_t(thumbImg->width)), std::min<int32_t>(zoom[1] + zoom[3], int32_t(thumbImg->height)), 1 };
            }
            else
                b.srcOffsets[1] = { int32_t(std::min<uint32_t>(thumbImg->width, 1280)), int32_t(std::min<uint32_t>(thumbImg->height, 720)), 1 };
            const int32_t x = int32_t(slot % kThumbCols * kThumbW), y = int32_t(slot / kThumbCols * kThumbH);
            b.dstOffsets[0] = { x, y, 0 };
            b.dstOffsets[1] = { x + int32_t(kThumbW), y + int32_t(kThumbH), 1 };
            vkCmdBlitImage(g_cmd, thumbImg->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_capAtlas.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        }
        CaptureProbeNow(rt && !rt->isDepth ? rt : nullptr, rt && rt->isDepth ? rt : g_capDepthRt, false);
        if (slot >= 0)
            std::fprintf(g_capLog, "%4u [slot %3d r%02d c%02d] %s\n", g_capStep, slot, slot / int(kThumbCols), slot % int(kThumbCols), text);
        else
            std::fprintf(g_capLog, "%4u [  no thumb   ] %s\n", g_capStep, text);
        ++g_capStep;
    }

    // ------------------------------------------------------------------ resolve
    // A resolve rectangle in render-target pixels (1x) for the given EDRAM info.
    VkRect2D EdramRect(const draw_util::ResolveEdramInfo& e, uint32_t originalBase, const draw_util::ResolveInfo& info, VkExtent2D limit)
    {
        uint32_t tileW, tileH;
        TileSize(xenos::MsaaSamples(e.msaa_samples), e.format_is_64bpp != 0, tileW, tileH);
        const uint32_t pitchTiles = std::max<uint32_t>(e.pitch_tiles, 1);
        const uint32_t rel = (e.base_tiles - originalBase) & (xenos::kEdramTileCount - 1);
        VkRect2D r;
        r.offset.x = int32_t((rel % pitchTiles) * tileW + info.coordinate_info.edram_offset_x_div_8 * 8);
        r.offset.y = int32_t((rel / pitchTiles) * tileH + info.coordinate_info.edram_offset_y_div_8 * 8);
        r.extent.width = std::min<uint32_t>(info.coordinate_info.width_div_8 * 8, limit.width - std::min<uint32_t>(r.offset.x, limit.width));
        r.extent.height = std::min<uint32_t>(info.height_div_8 * 8, limit.height - std::min<uint32_t>(r.offset.y, limit.height));
        return r;
    }

    // RB_DEPTH_CLEAR: 24-bit depth (unorm, or Xenos float 20e4 for D24FS8) in 8:31, stencil in 0:7.
    void ClearDepth(const draw_util::ResolveInfo& info)
    {
        RenderTarget* depth = GetRenderTarget(info.depth_original_base, kDepthFormat, info.depth_edram_info.pitch_tiles,
            xenos::MsaaSamples(info.depth_edram_info.msaa_samples));
        if (!depth) return;
        depth->depthFloat = info.depth_edram_info.format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8);
        const VkRect2D rect = EdramRect(info.depth_edram_info, info.depth_original_base, info, { depth->fullWidth, depth->fullHeight });
        if (!rect.extent.width || !rect.extent.height) return;
        AcquireTiles(depth, rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height);
        const uint32_t v = info.rb_depth_clear;
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        clear.clearValue.depthStencil.depth = info.depth_edram_info.format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8)
            ? xenos::Float20e4To32(v >> 8) : xenos::UNorm24To32(v >> 8);
        clear.clearValue.depthStencil.stencil = v & 0xFF;
        const VkClearRect clearRect{ rect, 0, 1 };
        BeginPass(nullptr, depth);
        vkCmdClearAttachments(g_cmd, 1, &clear, 1, &clearRect);
    }

    // Depth copy (resolve of the depth buffer to a k_24_8 / k_24_8_FLOAT texture, e.g. shadow maps).
    // The surface holds depth as R32_SFLOAT in 0..1, which is what the guest's fetch of a 24-bit
    // depth texture returns in .x. Vulkan can't copy depth to a color image directly: go through a
    // buffer.
    Buffer g_depthCopyBuf;
    int g_abPhase = 0; // 0 off, 1 frame A, 2 frame B, 3 done

    void ResolveDepth(const draw_util::ResolveInfo& info)
    {
    // PES_AB_AT=<seconds>: frame A normal, the next frame B with depth textures zeroed -> userdata/shots/ab_A.ppm, ab_B.ppm.
        static const bool skip = std::getenv("PES_DEBUG_NO_DEPTH_COPY") != nullptr; // A/B comparisons
        if (skip) return;
        const auto& e = info.depth_edram_info;
        RenderTarget* d = GetRenderTarget(info.depth_original_base, kDepthFormat, e.pitch_tiles, xenos::MsaaSamples(e.msaa_samples));
        if (!d) return;
        const VkRect2D src = EdramRect(e, info.depth_original_base, info, { d->fullWidth, d->fullHeight });
        if (!src.extent.width || !src.extent.height) return;
        AcquireTiles(d, src.offset.x, src.offset.y, src.extent.width, src.extent.height);

        const uint32_t destPitch = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
        const uint32_t destHeight = info.copy_dest_coordinate_info.height_aligned_div_32 * 32;
        Surface* s = GetResolveSurface(info.copy_dest_base, destPitch, destHeight, 4, VK_FORMAT_R32_SFLOAT);
        if (!s) return;
        s->rbSwapped = false;
        s->isDepth = true;
        SnapshotSurface(s);
        int32_t dstX, dstY;
        ResolveDestOffset(s, info.copy_dest_base, info, dstX, dstY);
        if (dstX < 0 || dstY < 0 || uint32_t(dstX) >= s->img.width || uint32_t(dstY) >= s->img.height) return;
        const uint32_t w = std::min<uint32_t>(src.extent.width, s->img.width - dstX);
        const uint32_t h = std::min<uint32_t>(src.extent.height, s->img.height - dstY);
        const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
        if (g_depthCopyBuf.size < bytes)
        {
            if (g_depthCopyBuf.buffer) WarnOnce("depth copy larger than the staging buffer");
            if (!g_depthCopyBuf.buffer && !CreateDeviceBuffer(g_depthCopyBuf, 2048ull * 2048 * 4,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
                return;
            if (g_depthCopyBuf.size < bytes) return;
        }
        Transition(d->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        // The buffer may still be read by the previous depth copy.
        VkBufferMemoryBarrier bb{ VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
        bb.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = g_depthCopyBuf.buffer;
        bb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
        VkBufferImageCopy c{};
        c.imageSubresource = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1 };
        c.imageOffset = { src.offset.x, src.offset.y, 0 };
        c.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(g_cmd, d->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_depthCopyBuf.buffer, 1, &c);
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageOffset = { dstX, dstY, 0 };
        if (g_abPhase == 2)
        {
            // A/B frame B: depth textures read 0 (as before depth copies existed).
            VkClearColorValue zero{};
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(g_cmd, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        }
        else
            vkCmdCopyBufferToImage(g_cmd, g_depthCopyBuf.buffer, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        static uint32_t logs = 0;
        if (logs++ < 6)
            std::printf("[render] depth copy: depth base %u pitch %u msaa %u rect %d,%d %ux%u -> 0x%08X (surface 0x%08X) at %d,%d, dest fmt %u\n",
                d->base, d->pitchTiles, uint32_t(d->msaa), src.offset.x, src.offset.y, w, h, info.copy_dest_base, s->base, dstX, dstY,
                uint32_t(info.copy_dest_info.copy_dest_format));
        CaptureStep(nullptr, "DEPTH RESOLVE depth base %u pitch %u msaa %u rect %d,%d %ux%u -> 0x%08X (surface 0x%08X) at %d,%d dest fmt %u%s",
            d->base, d->pitchTiles, uint32_t(d->msaa), src.offset.x, src.offset.y, w, h, info.copy_dest_base, s->base, dstX, dstY,
            uint32_t(info.copy_dest_info.copy_dest_format), info.IsClearingDepth() ? " +depthclear" : "");
    }

    // CPU view of resolves. Mirrored resolve memory holds the texels in the order our GPU path
    // reads back correctly ([c3 c0 c1 c2] for swap+8in32 resolves). PES's CPU code that consumes
    // resolve output (XG untile sub_82B1A9D0, then an ARGB->RGBA byte rotation and the kit
    // compositor) expects the EDRAM channel order as-is: with the GPU order a jersey number
    // tile's transparent background (EDRAM 00 F0 F0 F0) came out opaque cyan on every in-play
    // shirt (2026-11-07). So when the XG untile reads a mirrored resolve, that memory is
    // rewritten in raw EDRAM order first. Registry: renderer thread adds, guest threads consume.
    struct MirroredResolve { uint32_t size; bool rbSwapped; xenos::Endian endian; bool fromLargeSource; };
    std::mutex g_mirroredMutex;
    std::map<uint32_t, MirroredResolve> g_mirrored; // physical base -> layout of the stored texels
    uint64_t g_cpuViewConversions = 0;

    // Stores a 32bpp surface into guest memory as the console's resolve would: Xenos tiled layout
    // (pitch = surface pitch), dest endianness, red/blue as the resolve left them in memory. The
    // stored bytes are exactly what GetTexture turns back into the surface's texels. Synchronous
    // GPU readback (experiment, see PES_RESOLVE_WRITEBACK).
    uint64_t g_writeBacks = 0;
    void WriteBackSurface(Surface* s, xenos::Endian128 endian, bool fromLargeSource = false)
    {
        const uint32_t w = s->img.width, h = s->img.height;
        static Buffer rb;
        static VkDeviceSize rbSize = 0;
        if (rbSize < VkDeviceSize(w) * h * 4)
        {
            if (rb.buffer) DestroyBuffer(rb);
            rbSize = VkDeviceSize(256) * 256 * 4;
            if (!CreateBuffer(rb, rbSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) { rbSize = 0; return; }
        }
        Transition(s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(g_cmd, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &copy);
        EndFrame(true); // the readback below needs the GPU done
        BeginFrame();
        const xenos::Endian e = endian == xenos::Endian128::k8in16 ? xenos::Endian::k8in16
            : endian == xenos::Endian128::k8in32 ? xenos::Endian::k8in32
            : endian == xenos::Endian128::k16in32 ? xenos::Endian::k16in32 : xenos::Endian::kNone;
        MemoryWatch::HostWillWrite(0xA0000000u + s->base, s->size); // our own store into the watched pages
        uint8_t* guest = g_xeMemory.TranslatePhysical(s->base);
        const uint32_t pitch = s->pitch;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t off = uint32_t(texture_address::Tiled2D(int32_t(x), int32_t(y), pitch, 2));
                if (s->base + off + 4 > kSharedMemorySize) continue;
                uint8_t t[4];
                std::memcpy(t, rb.mapped + (size_t(y) * w + x) * 4, 4);
                // PES_WRITEBACK_ORDER=rgba (experiment): EDRAM channel order as-is, no swaps.
                static const bool rawOrder = [] { const char* o = std::getenv("PES_WRITEBACK_ORDER"); return o && !std::strcmp(o, "rgba"); }();
                if (!rawOrder)
                {
                    if (s->rbSwapped) std::swap(t[0], t[2]);
                    SwapDwords(t, 4, e); // the swaps are their own inverse
                }
                std::memcpy(guest + off, t, 4);
            }
        ++g_writeBacks;
        {
            std::lock_guard lock(g_mirroredMutex);
            g_mirrored[s->base] = { s->size, s->rbSwapped, e, fromLargeSource };
        }
        SnapshotSurface(s); // the guest bytes now match the surface
        s->writtenBackGen = s->generation;
        // PES_WATCH_RESOLVE_READ=<W>x<H> (debug): report the first access to the first mirrored
        // resolve of that shape at or above 0x18000000 (who consumes resolved names/numbers).
        static const char* watchEnv = std::getenv("PES_WATCH_RESOLVE_READ");
        static bool watched = false;
        if (watchEnv && !watched && s->base >= 0x18000000u)
        {
            uint32_t ww = 0, wh = 0;
            if (std::sscanf(watchEnv, "%ux%u", &ww, &wh) == 2 && s->pitch == ww && s->height == wh)
            {
                watched = true;
                std::printf("[render] watching first access to resolve 0x%08X (%ux%u), rbSwapped %d, dest endian %u, EDRAM texels:", s->base, ww, wh,
                    int(s->rbSwapped), uint32_t(endian));
                for (int i = 0; i < 32; ++i) std::printf("%s%02X", (i % 4) ? "" : " ", rb.mapped[i]);
                std::printf("\n");
                MemoryWatch::ReportAccess(s->base, s->size);
            }
        }
    }

    void Resolve()
    {
        ScopedTimer resolveTimer{ g_time.resolve };
        draw_util::ResolveInfo info;
        if (!draw_util::GetResolveInfo(g_regs, g_xeMemory, g_trace, 1, 1, false, false, info))
        {
            WarnOnce("GetResolveInfo failed");
            return;
        }
        BeginFrame();
        MarkGpu(3);
        struct ResolveMark { ~ResolveMark() { MarkGpu(1); } } resolveMark;
        if (info.IsCopyingDepth())
        {
            ResolveDepth(info);
            if (info.IsClearingDepth()) ClearDepth(info);
            return;
        }
        const auto edram = info.color_edram_info;
        const VkFormat rtFormat = ColorRtFormat(xenos::ColorRenderTargetFormat(edram.format));
        RenderTarget* rt = GetRenderTarget(info.color_original_base, rtFormat, edram.pitch_tiles, xenos::MsaaSamples(edram.msaa_samples));
        if (!rt) return;
        const uint32_t kRtWidth = rt->fullWidth, kRtHeight = rt->fullHeight; // limits below: this target (logical)

        // Source rectangle in render-target pixels (1x).
        uint32_t tileW, tileH;
        TileSize(xenos::MsaaSamples(edram.msaa_samples), edram.format_is_64bpp != 0, tileW, tileH);
        const uint32_t pitchTiles = std::max<uint32_t>(edram.pitch_tiles, 1);
        const uint32_t rel = (edram.base_tiles - info.color_original_base) & (xenos::kEdramTileCount - 1);
        const int32_t srcX = int32_t((rel % pitchTiles) * tileW + info.coordinate_info.edram_offset_x_div_8 * 8);
        const int32_t srcY = int32_t((rel / pitchTiles) * tileH + info.coordinate_info.edram_offset_y_div_8 * 8);
        uint32_t width = info.coordinate_info.width_div_8 * 8, height = info.height_div_8 * 8;

        // Destination surface.
        const auto destFormat = xenos::TextureFormat(info.copy_dest_info.copy_dest_format);
        const FormatInfo* destInfo = FormatInfo::Get(destFormat);
        const uint32_t bpp = std::max<uint32_t>(destInfo ? destInfo->bits_per_pixel / 8 : 4, 1);
        const uint32_t destPitch = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
        const uint32_t destHeight = info.copy_dest_coordinate_info.height_aligned_div_32 * 32;
        const uint32_t destBase = info.copy_dest_base;
        Surface* s = GetResolveSurface(destBase, destPitch, destHeight, bpp, rtFormat);
        if (!s) return;
        // PES_LOG_RESOLVES=<seconds>: log 150 resolves starting at that time.
        static uint32_t resolveLogs = 0;
        static const double logFrom = [] { const char* e = std::getenv("PES_LOG_RESOLVES"); return e ? std::atof(e) : -1.0; }();
        static const auto logStart = std::chrono::steady_clock::now();
        static bool logging = false; // starts at the first screen-sized resolve after logFrom
        if (!logging && logFrom >= 0 && width >= 640 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - logStart).count() >= logFrom)
        {
            logging = true;
            std::printf("[resolve] ---- logging from frame %llu\n", (unsigned long long)g_frame);
        }
        if (logging && width >= 640 && resolveLogs++ < 60)
            std::printf("[resolve] f%llu src rt base %u (edram base %u, pitch %u tiles, msaa %u) rect %d,%d %ux%u -> 0x%08X (surface 0x%08X pitch %u) off %u,%u%s%s\n",
                (unsigned long long)g_frame, info.color_original_base, uint32_t(edram.base_tiles), uint32_t(edram.pitch_tiles), uint32_t(edram.msaa_samples),
                srcX, srcY, width, height, destBase, s ? s->base : 0, destPitch,
                info.copy_dest_coordinate_info.offset_x_div_8 * 8, info.copy_dest_coordinate_info.offset_y_div_8 * 8,
                info.IsClearingColor() ? " +clear" : "", info.IsClearingDepth() ? " +depthclear" : "");
        int32_t dstX, dstY;
        ResolveDestOffset(s, destBase, info, dstX, dstY);
        if (uint32_t(srcX) >= kRtWidth || uint32_t(srcY) >= kRtHeight)
        {
            WarnOnce("resolve source outside the render target image");
            return;
        }
        // The resolved (and possibly cleared) tiles' current content.
        AcquireTiles(rt, srcX, srcY, std::min<uint32_t>(width, kRtWidth - srcX), std::min<uint32_t>(height, kRtHeight - srcY));
        width = std::min<uint32_t>(width, std::min<int32_t>(s->img.width - dstX, kRtWidth - srcX));
        height = std::min<uint32_t>(height, std::min<int32_t>(s->img.height - dstY, kRtHeight - srcY));

        s->rbSwapped = info.copy_dest_info.copy_dest_swap != 0;
        SnapshotSurface(s);
        if (g_capArmed && g_capState == CaptureState::Active && destBase >= g_capArmMin && s->pitch == g_capArmW && s->height == g_capArmH)
        {
            g_capArmHit = true;
            g_capArmHitAddr = destBase;
        }
        // PES_LOG_SMALL_RESOLVES: every resolve into a small surface (kits, faces, names).
        // PES_LOG_SMALL_RESOLVES=<min address, hex> (1 = all): only resolves at or above that address.
        static const char* logSmallEnv = std::getenv("PES_LOG_SMALL_RESOLVES");
        static const uint32_t logSmallMin = logSmallEnv ? uint32_t(std::strtoul(logSmallEnv, nullptr, 16)) : 0;
        const bool logSmall = logSmallEnv && destBase >= (logSmallMin == 1 ? 0u : logSmallMin);
        if (logSmall && s->pitch <= 256 && s->height <= 256)
            std::printf("[small-resolve-raw] RB_COPY_DEST_BASE 0x%08X -> adjusted 0x%08X, DEST_PITCH 0x%08X, DEST_INFO 0x%08X, "
                "RB_COPY_CONTROL 0x%08X, window offset 0x%08X, edram offset %u,%u, dest offset %u,%u, extent 0x%08X+0x%X\n",
                g_regs[XE_GPU_REG_RB_COPY_DEST_BASE], info.copy_dest_base, g_regs[XE_GPU_REG_RB_COPY_DEST_PITCH],
                g_regs[XE_GPU_REG_RB_COPY_DEST_INFO], g_regs[XE_GPU_REG_RB_COPY_CONTROL], g_regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET],
                info.coordinate_info.edram_offset_x_div_8 * 8, info.coordinate_info.edram_offset_y_div_8 * 8,
                info.copy_dest_coordinate_info.offset_x_div_8 * 8, info.copy_dest_coordinate_info.offset_y_div_8 * 8,
                info.copy_dest_extent_start, info.copy_dest_extent_length);
        if (logSmall && s->pitch <= 256 && s->height <= 256)
            std::printf("[small-resolve] f%llu -> 0x%08X (surface 0x%08X %ux%u) at %d,%d from rt %u fmt %d pitch %u msaa %u src %d,%d %ux%u%s\n",
                (unsigned long long)g_frame, destBase, s->base, s->pitch, s->height, dstX, dstY, rt->base, int(rt->format), rt->pitchTiles,
                uint32_t(rt->msaa), srcX, srcY, width, height, info.IsClearingColor() ? " +clear" : "");
        if (width && height && dstX >= 0 && dstY >= 0)
        {
            Transition(rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            Transition(s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy copy{};
            copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.srcOffset = { srcX, srcY, 0 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.dstOffset = { dstX, dstY, 0 };
            copy.extent = { width, height, 1 };
            vkCmdCopyImage(g_cmd, rt->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->img.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
            // On the console a resolve writes guest memory, and the game relies on that: PES copies
            // resolved player kits into their final textures on the CPU (one team's kits were
            // missing - flat silhouettes - until resolves were mirrored to memory, 2026-11-07).
            // Mirrored here: one-off resolves (not repeated in consecutive frames: kits, numbers,
            // names built at load time) up to 256x256, synchronously (GPU readback). Per-frame
            // resolves (UI, post-processing) stay GPU-only so frames do not stall.
            // PES_RESOLVE_WRITEBACK=0: never, =all: every small resolve.
            static const char* wbEnv = std::getenv("PES_RESOLVE_WRITEBACK");
            static const int wbMode = !wbEnv ? 1 : !std::strcmp(wbEnv, "0") ? 0 : !std::strcmp(wbEnv, "all") ? 2 : 1;
            // Decided once per surface and frame (a kit may take several resolves in one frame).
            if (s->lastResolveFrame != g_frame || s->lastResolveFrame == 0)
            {
                // Resolved again within kPeriod frames: a per-frame effect (in-play kit LOD atlases are
                // re-rendered in rotation, one every 4 frames: with a window of 2 each was written back
                // synchronously, ~7 GPU syncs per frame). PES_WB_PERIOD=<frames> (default 8).
                static const uint64_t kPeriod = [] { const char* e = std::getenv("PES_WB_PERIOD"); return e ? uint64_t(std::atoi(e)) : 8ull; }();
                s->mirror = s->lastResolveFrame == 0 || s->lastResolveFrame + kPeriod < g_frame;
                s->lastResolveFrame = g_frame + (g_frame == 0); // 0 means "never"
            }
            if (wbMode && (s->mirror || wbMode == 2) && s->pitch <= 256 && s->height <= 256 && s->bytesPerPixel == 4 &&
                s->img.format == VK_FORMAT_R8G8B8A8_UNORM)
            {
                static uint32_t clsLogs = 0;
                if (destBase >= 0x18000000u && clsLogs++ < 400)
                    std::printf("[render] mirrored resolve 0x%08X %ux%u, largest source %ux%u\n", destBase, s->pitch, s->height, rt->lastSourceW, rt->lastSourceH);
                WriteBackSurface(s, info.copy_dest_info.copy_dest_endian, rt->lastSourceW >= 1024); // kits are 1024x512; number fonts 512x256
                {
                    std::lock_guard lock(g_wbStatsMutex);
                    ++g_wbStats[(uint64_t(destBase) << 32) | (s->pitch << 16) | s->height];
                }
            }
            rt->lastSourceW = rt->lastSourceH = 0; // largest source since the previous resolve
        }

        if (info.IsClearingColor())
        {
            const uint32_t c = info.rb_color_clear;
            VkClearAttachment clear{};
            clear.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clear.colorAttachment = 0;
            clear.clearValue.color.float32[0] = float(c & 0xFF) / 255.0f;
            clear.clearValue.color.float32[1] = float((c >> 8) & 0xFF) / 255.0f;
            clear.clearValue.color.float32[2] = float((c >> 16) & 0xFF) / 255.0f;
            clear.clearValue.color.float32[3] = float(c >> 24) / 255.0f;
            // Only the resolved rectangle (the whole 2048x2048 image per resolve was very slow).
            const uint32_t w = info.coordinate_info.width_div_8 * 8, h = info.height_div_8 * 8;
            VkClearRect rect{};
            rect.rect.offset = { srcX, srcY };
            rect.rect.extent = { std::min<uint32_t>(w, kRtWidth - srcX), std::min<uint32_t>(h, kRtHeight - srcY) };
            rect.layerCount = 1;
            if (rect.rect.extent.width && rect.rect.extent.height)
            {
                BeginPass(rt, nullptr);
                vkCmdClearAttachments(g_cmd, 1, &clear, 1, &rect);
            }
        }
        if (info.IsClearingDepth())
            ClearDepth(info);
        // PES_CAPTURE_SURFACES=1: resolve thumbnails show the destination surface (whole image).
        static const bool capSurfaces = std::getenv("PES_CAPTURE_SURFACES") != nullptr;
        if (capSurfaces) g_capThumbImage = &s->img;
        CaptureStep(rt, "RESOLVE rt %u fmt %d pitch %u msaa %u src %d,%d %ux%u -> 0x%08X (surface 0x%08X) at %d,%d | dest fmt %u exp bias %d swap %u sample %u%s%s",
            rt->base, int(rt->format), rt->pitchTiles, uint32_t(rt->msaa), srcX, srcY, width, height, destBase, s->base, dstX, dstY,
            uint32_t(info.copy_dest_info.copy_dest_format), int32_t(info.copy_dest_info.copy_dest_exp_bias),
            uint32_t(info.copy_dest_info.copy_dest_swap), uint32_t(g_regs.Get<reg::RB_COPY_CONTROL>().copy_sample_select),
            info.IsClearingColor() ? " +clear" : "", info.IsClearingDepth() ? " +depthclear" : "");
    }

    // ------------------------------------------------------------------ draw
    void DrawImpl2(uint32_t indexBase, uint32_t indexSizeDword);
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
                        void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override
                        {
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
                            interpreter.temp_registers()[0] = float((uint32_t(int32_t(idx(r * 3 + k)) + baseIndex)) & 0xFFFFFF);
                            interpreter.Execute();
                            if (!sink.has) { cornersFound = false; break; }
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
        const bool restart = restartEnabled && !convert &&
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
        const VkPipeline pipeline = GetPipeline(pd);
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
                CaptureNote("      claim: color %s %d,%d-%d,%d (tile %ux%u)%s | depth %s %d,%d-%d,%d%s", colorMask ? "yes" : "no", x0, y0, x1, cy1,
                    rt->tileW, rt->tileH, colorNoCopy ? " noCopy" : "", depthRt ? "yes" : "no", x0, y0, x1, dy1, depthNoCopy ? " noCopy" : "");
                if (colorMask && cy1 > y0) AcquireTiles(rt, x0, y0, uint32_t(x1 - x0), uint32_t(cy1 - y0), colorNoCopy);
                if (depthRt && dy1 > y0) AcquireTiles(depthRt, x0, y0, uint32_t(x1 - x0), uint32_t(dy1 - y0), depthNoCopy);
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
        BeginPass(rt, depthRt);
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
        MarkGpu(0);
        ++g_stats.issued;
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

    // ------------------------------------------------------------------ debug readback
    uint64_t g_dumpSurfacesFrame = ~0ull;

    // Reads an RGBA8 image back and writes it as a binary PPM (debugging).
    void DumpImage(Image& img, uint32_t width, uint32_t height, const std::string& path, bool swapRedBlue = false)
    {
        const uint32_t bpp = img.format == VK_FORMAT_R8G8B8A8_UNORM ? 4 : img.format == VK_FORMAT_R8_UNORM ? 1 : 0;
        if (!bpp) return;
        Buffer rb;
        if (!CreateBuffer(rb, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return;
        BeginFrame();
        Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { width, height, 1 };
        vkCmdCopyImageToBuffer(g_cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buffer, 1, &copy);
        EndFrame(true); // the readback below needs the GPU done
        if (FILE* f = std::fopen(path.c_str(), "wb"))
        {
            std::fprintf(f, "P6\n%u %u\n255\n", width, height);
            for (uint32_t i = 0; i < width * height; ++i)
            {
                const uint8_t* px = rb.mapped + size_t(i) * bpp;
                uint8_t rgb[3] = { px[0], bpp == 4 ? px[1] : px[0], bpp == 4 ? px[2] : px[0] };
                if (swapRedBlue) std::swap(rgb[0], rgb[2]);
                std::fwrite(rgb, 1, 3, f);
            }
            std::fclose(f);
        }
        // Small images: the alpha channel too, as <path>.alpha.pgm.
        if (bpp == 4 && width * height <= 1024 * 1024)
            if (FILE* f = std::fopen((path + ".alpha.pgm").c_str(), "wb"))
            {
                std::fprintf(f, "P5\n%u %u\n255\n", width, height);
                for (uint32_t i = 0; i < width * height; ++i) std::fputc(rb.mapped[size_t(i) * 4 + 3], f);
                std::fclose(f);
            }
        DestroyBuffer(rb);
        std::printf("[render] wrote %s\n", path.c_str());
    }

    void DumpAllSurfaces()
    {
        for (auto& [key, rt] : g_rts)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "userdata/rt_base%u.ppm", rt->base);
            DumpImage(rt->img, 1280, 736, name);
        }
        for (auto& sf : g_surfaces)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "userdata/surface_%08X.ppm", sf->base);
            DumpImage(sf->img, sf->img.width, sf->img.height, name);
        }
        for (auto& [key, tex] : g_textures)
        {
            if (tex->uploadedFrame + 1 < g_frame) continue;
            char name[96];
            std::snprintf(name, sizeof(name), "userdata/texture_%08X.ppm", key[0]);
            DumpImage(tex->img, tex->img.width, tex->img.height, name);
        }
    }

    // ------------------------------------------------------------------ init helpers
    bool CreateNullImage(Image& img, VkImageType type, VkImageViewType viewType, uint32_t layers)
    {
        if (!CreateImage(img, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                type, viewType, layers))
            return false;
        return true;
    }
}

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

    bool Init()
    {
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
        BeginFrame();
        EndPass();
        Surface* s = FindSurfaceAt(frontBufferPhysical & 0x1FFFF000u);
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
        EndFrame();
        ++g_frame;
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
            std::printf("[render] last 120 frames (ms): resolves (CPU) %.0f\n", m.resolve / 1000.0);
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
            std::printf("[render] last 120 frames GPU (ms): draws %.0f, resolves %.0f, uploads %.0f, other %.0f | %u render pass begins, %u barriers, %llu EDRAM tiles moved (%llu not copied: overwritten), %zu render targets\n",
                g_gpuMs[0], g_gpuMs[1], g_gpuMs[2], g_gpuMs[3], g_passBegins, g_barriers, (unsigned long long)g_tileTransfers, (unsigned long long)g_tilesSkipped, g_rts.size());
            std::memset(g_gpuMs, 0, sizeof(g_gpuMs));
            g_passBegins = g_barriers = 0;
            g_tileTransfers = g_tilesSkipped = 0;
            g_time = {};
            std::fflush(stdout);
            const uint32_t issued = g_stats.issued;
            g_stats = {};
            g_stats.issued = issued > 12 ? 13 : issued; // keep the first-draws log quiet
        }
        if (!present) return false;
        {
            ScopedTimer presentTimer{ g_time.present };
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
