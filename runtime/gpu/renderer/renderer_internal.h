// Renderer internals shared by runtime/gpu/renderer.cpp and runtime/gpu/renderer/*.cpp.
// Everything that used to be the anonymous namespace of renderer.cpp: types and globals here
// (globals are C++17 inline variables, one instance program-wide), function bodies in the
// files listed below. Pure move of the 2026-11-17 split - no behaviour change.
//   vulkan_core.cpp  Vulkan basics: memory, buffers, images, layout transitions, frame slots, GPU timestamps.
//   edram.cpp        EDRAM model: render targets, tile ownership, depth->color transfers, lazy clears, render passes.
//   surfaces.cpp     Resolve surfaces: guest-address ranges that resolves copy into and textures sample.
//   textures.cpp     Textures: formats, BC decode, guest texture upload and cache, composed textures, samplers.
//   pipelines.cpp    Shaders and pipelines: modules, layouts, blend state, pipeline cache, constants, descriptor sets.
//   capture.cpp      Debug capture (PES_CAPTURE / control "capture"): per-draw thumbnails and step log.
//   resolve.cpp      Resolves (EDRAM -> surfaces): color/depth copies, depth clears, write-back to guest memory.
//   draw.cpp         Draws: index/vertex setup, rectangle lists, state -> pipeline, clears, the draw itself.
//   ../renderer.cpp  Public entry points: Init, WriteRegister, Draw, Swap (frame end, stats, PES_CONTROL).
#pragma once

#include "../renderer.h"
#include "../gpu.h"
#include "../d3d_wait.h"
#include "../../debug/process_memory.h"
#include "../memory_watch.h"
#include "../../debug/write_watch.h"

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

namespace RendererImpl
{
    using namespace xe::gpu;
    namespace xenos = xe::gpu::xenos;
    using Translator = SpirvShaderTranslator;
    // ------------------------------------------------------------------ basics
    inline const Video::VulkanContext* g_vk = nullptr;
    inline VkDevice g_dev = VK_NULL_HANDLE;
    inline bool g_active = false;
    inline uint64_t g_frame = 0;
    inline RegisterFile g_regs;
    inline xe::Memory g_xeMemory;
    inline TraceWriter g_trace;
    inline xe::gpu::DrawExtentEstimator g_extentEstimator(g_regs, g_xeMemory, &g_trace);
    inline std::unique_ptr<Translator> g_translator;
    constexpr uint32_t kSharedMemorySize = 512u << 20;
    // Debug statistics, printed every 120 frames.
    struct Stats { uint32_t issued, resolves, noVs, noRaster, noColor, noModule, noViewport, noPipeline, badPrim, other; };
    inline Stats g_stats{};
    // Time spent per phase (microseconds), printed with the stats.
    struct Timing { uint64_t vertexCopy, vertexBytes, textures, textureUploads, drawTotal, resolve, submitWait, modules, pipelines, newModules, newPipelines, extentEstimate, rectCorners, rectDraws, endRecord, submit, fenceWait, present, gpuSpanUs, frames; };
    inline Timing g_time{};
    // GPU timestamps: each mark closes an interval since the previous mark and attributes it
    // to a category (0 draw, 1 resolve, 2 texture upload, 3 other: barriers, pass changes, gaps).
    inline VkQueryPool g_tsPool = VK_NULL_HANDLE;
#ifdef __APPLE__
    // Metal counter sample buffers hold at most 32 KB (4096 timestamps); a bigger pool makes
    // MoltenVK fall back to emulated timestamps that always read 0.
    constexpr uint32_t kMaxTimestamps = 4096;
#else
    constexpr uint32_t kMaxTimestamps = 16384;
#endif
    constexpr uint32_t kFramesInFlight = 2;
    inline uint32_t g_tsCount = 0;
    inline uint8_t* g_tsCategory = nullptr; // the current frame slot's array
    inline uint64_t* g_tsShader = nullptr;   // the current frame slot's shader per timestamp (GPU time per shader, see MarkGpu)
    inline uint64_t g_markShader = 0;        // set around a draw: its pixel shader hash
    inline std::map<uint64_t, std::pair<double, uint32_t>> g_shaderGpuMs; // pixel shader -> GPU ms, draws (last 120 frames)
    inline double g_gpuMs[5] = {}; // by category: 0 draws (render passes), 1 resolves, 2 uploads, 3 other, 4 EDRAM tile transfers
    inline uint32_t g_passBegins = 0, g_barriers = 0;
    // Render pass statistics (printed every 120 frames): why passes end, how many draws they
    // hold, and how much of the attachment they touch versus what they load + store. On a
    // tile-based GPU (iPad) every pass loads and stores its whole framebuffer.
    // g_passEndContext names the operation that is ending the pass (set by PassEndContext).
    inline const char* g_passEndContext = nullptr;
    struct PassEndContext
    {
        const char* saved;
        explicit PassEndContext(const char* what) : saved(g_passEndContext) { g_passEndContext = what; }
        ~PassEndContext() { g_passEndContext = saved; }
    };
    struct PassStats
    {
        std::map<std::string, uint32_t> endReasons;
        uint32_t byDraws[4] = {}; // 0, 1, 2-4, 5+ draws
        uint64_t framebufferPixels = 0, touchedPixels = 0, draws = 0;
    };
    inline PassStats g_passStats;
    inline uint32_t g_passDraws = 0;
    inline int32_t g_passX0 = 0, g_passY0 = 0, g_passX1 = 0, g_passY1 = 0; // union of the draws' scissors
    inline VkExtent2D g_passExtent{};
    inline VkExtent2D g_passNeed{}; // union of the area the current pass's draws / clears needed (BeginPass)
    void MarkGpu(uint8_t category, bool frameBoundary = false);
    struct ScopedTimer
    {
        uint64_t& sink;
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        ~ScopedTimer() { sink += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count(); }
    };
    constexpr uint32_t kRtWidth = 2048, kRtHeight = 2048, kRtMaxHeight = 4096;
    void WarnOnce(const std::string& what);
    uint32_t FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer = 0);
    // Vulkan memory in use, by kind (bytes as allocated), for the [mem] report.
    struct MemCounters { uint64_t hostBuffers = 0, deviceBuffers = 0, images = 0, imageCount = 0, imagesFreed = 0, poolExhausted = 0; };
    inline MemCounters g_mem;
    bool AllocateMemory(const VkMemoryAllocateInfo& ai, VkDeviceMemory* out, const char* what);
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
    void DestroyBuffer(Buffer& b);
    bool CreateBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage);
    bool CreateDeviceBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage);
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
        uint32_t width = 0, height = 0, layers = 1, depth = 1, levels = 1;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageView attachmentView = VK_NULL_HANDLE; // 2D, identity (render targets)
        std::map<uint32_t, VkImageView> views;      // sampled views by packed swizzle
        VkDeviceSize allocSize = 0;
    };
    void DestroyImage(Image& img);
    bool CreateImage(Image& img, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImageType type = VK_IMAGE_TYPE_2D, VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY, uint32_t layers = 1, uint32_t depth = 1, uint32_t levels = 1);
    VkImageView SampledView(Image& img, uint32_t packedSwizzle);
    constexpr uint32_t PackSwizzle(VkComponentSwizzle r, VkComponentSwizzle g, VkComponentSwizzle b, VkComponentSwizzle a)
    {
        return uint32_t(r) | (uint32_t(g) << 3) | (uint32_t(b) << 6) | (uint32_t(a) << 9);
    }
    constexpr uint32_t kIdentitySwizzle = PackSwizzle(VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A);
    constexpr uint32_t kSwapRBSwizzle = PackSwizzle(VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A);
    // ------------------------------------------------------------------ frame
    inline VkCommandPool g_cmdPool = VK_NULL_HANDLE;
    inline VkCommandBuffer g_cmd = VK_NULL_HANDLE;
    inline VkFence g_fence = VK_NULL_HANDLE;
    inline VkDescriptorPool g_descriptorPool = VK_NULL_HANDLE; // reset every frame
    // Per-frame descriptor set reuse. Sets come from the frame slot's pool and refer to the slot's
    // uniform buffer, so both caches are cleared whenever that pool is reset (BeginFrame).
    inline std::unordered_map<uint64_t, VkDescriptorSet> g_textureSetCache; // XXH3 of layout + views + samplers
    inline std::unordered_map<uint32_t, VkDescriptorSet> g_constantSetCache; // (vertex float range, pixel float range)
    inline bool g_dynamicConstants = true; // constant buffers are UNIFORM_BUFFER_DYNAMIC (one set per frame, offsets per draw)
    inline uint64_t g_setsAllocated = 0, g_setsReused = 0;
    inline bool g_recording = false;
    inline Pool g_uniforms, g_uploads;
    // Guest physical memory mirror in VRAM. Each frame, every 4 KB page a draw reads is
    // copied (at most once per frame) through the upload pool into it, by g_uploadCmd,
    // which is submitted ahead of the frame's command buffer.
    inline Buffer g_shared;
    inline VkCommandBuffer g_uploadCmd = VK_NULL_HANDLE;
    inline std::vector<uint64_t> g_pagesCopied((512u << 20) / 4096 / 64);
    inline std::vector<uint64_t> g_pageHash((512u << 20) / 4096); // XXH3 of each page as copied this frame
    inline uint64_t g_pagesRecopied = 0;
    inline uint64_t g_texHashUs = 0, g_texHashBytes = 0, g_texSetUs = 0, g_constUs = 0; // stats
    inline std::mutex g_wbStatsMutex;
    inline std::map<uint64_t, uint32_t> g_wbStats; // synchronous resolve write-backs per (address, size), stats
    // Persistent vertex pages (2026-11-08): a page stays valid in the GPU mirror until the CPU writes
    // it (MemoryWatch page protection records the write) - no per-frame re-copy, no hashing.
    inline std::vector<uint64_t> g_pagesValid((512u << 20) / 4096 / 64);
    inline std::vector<uint32_t> g_pageCopyEpoch((512u << 20) / 4096);
    // PES_VB_HASH=1 / control "vb hash|track": the previous scheme (re-copy every frame, XXH3 of reused pages).
    inline bool g_vbHash = std::getenv("PES_VB_HASH") != nullptr;
    inline std::vector<VkBufferCopy> g_pendingCopies;
    // Rectangle lists (D3D clears, screen-space quads): the 4th corner of each rectangle is
    // computed on the CPU and the expanded vertices are written to a runtime-owned region of
    // guest physical memory (in the VRAM mirror only), which the draw's fetch constants point at.
    inline uint32_t g_rectScratch = 0; // offset in g_shared (identity mode: a runtime-owned physical address)
    constexpr uint32_t kRectScratchSize = 1u << 20;
    inline uint32_t g_rectScratchUsed = 0;
    // Vertex heap (2026-11-09). g_shared used to mirror all 512 MB of guest physical memory at
    // identical offsets, but in a match only ~32 MB of pages ever hold vertex data, spread over
    // ~434 MB. Now each guest range a draw fetches vertices from gets its own place in a compact
    // heap, and the draw's fetch constants (our uploaded copy) point there. Ranges are page-aligned;
    // a range that overlaps existing ones is merged into a new, larger range (re-copied; the old
    // space stays unused until the next reset). The heap is reset at a frame start when it is 3/4
    // full or a draw did not fit (the previous frame's reads are ordered before the next frame's
    // copies by the barrier at the start of g_uploadCmd). PES_VERTEX_IDENTITY=1: old 512 MB mirror.
    inline const bool g_vertexIdentity = std::getenv("PES_VERTEX_IDENTITY") != nullptr;
    inline VkDeviceSize g_sharedBytes = 0; // size of g_shared (identity: 512 MB)
    struct VertexRange { uint32_t end; uint32_t heap; }; // guest [start, end) -> heap offset of start
    inline std::map<uint32_t, VertexRange> g_vertexRanges;      // by guest start address
    constexpr uint32_t kVertexHeapStart = 4096 + kRectScratchSize; // [4 KB, 4 KB + 1 MB): rect scratch
    inline uint32_t g_vertexHeapTop = kVertexHeapStart;
    inline bool g_vertexHeapFull = false;
    inline uint64_t g_vertexHeapResets = 0, g_vertexRangeMerges = 0;
    uint32_t MapVertexRange(uint32_t start, uint32_t end);
    void MaybeResetVertexHeap();
    struct RenderTarget;
    // The open render pass (color and/or depth target), if any.
    inline RenderTarget* g_passColor = nullptr;
    inline RenderTarget* g_passDepth = nullptr;
    inline bool g_inPass = false;
    VkAccessFlags AccessFor(VkImageLayout l);
    void EndPass(const char* why = "other");
    void Transition(Image& img, VkImageLayout to);
    void ClearImage(Image& img, const VkClearColorValue& color);
    void ResetDepthToColorPool();
    // ------------------------------------------------------------------ frames in flight
    // Two frame slots: while the GPU executes frame N, the CP records frame N+1. Each slot owns
    // its command buffers, fence, descriptor pools, timestamp pool and host-visible
    // upload/uniform buffers; BeginFrame switches the globals (g_cmd, g_uploads, ...) to the next
    // slot after waiting for the frame that last used it (two frames back). Everything else is
    // GPU-only and ordered by barriers (a barrier also orders against earlier submissions).
    // GPU time per kind of render pass ([passgpu] line): every pass records the timestamps written
    // just before it begins and just after it ends, and a label (target, size, pixel shaders).
    // Works on Apple too, where timestamps exist only at pass edges.
    struct PassSpan { uint32_t begin, end, label, draws; };
    inline std::vector<std::string> g_passLabels;                    // label id -> text
    inline std::unordered_map<uint64_t, uint32_t> g_passLabelIds;   // pass key -> label id
    struct PassGpu { double ms = 0; uint32_t passes = 0; uint64_t draws = 0; };
    inline std::unordered_map<uint32_t, PassGpu> g_passGpu;          // label id -> last 120 frames
    inline std::vector<PassSpan>* g_passSpans = nullptr;             // the current frame slot's spans
    inline bool g_noPassGpu = std::getenv("PES_NO_PASSGPU") != nullptr; // PES_CONTROL "passgpu off": no [passgpu] spans (A/B)
    inline uint32_t g_passBeginTs = UINT32_MAX;                      // timestamp index before the open pass
    inline uint64_t g_passPs[3] = {};                                // first distinct pixel shaders of the open pass
    struct FrameSlot
    {
        std::vector<PassSpan> passSpans;
        VkCommandBuffer cmd = VK_NULL_HANDLE, uploadCmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkQueryPool timestamps = VK_NULL_HANDLE;
        Buffer uniforms, uploads;
        std::vector<uint8_t> tsCategory = std::vector<uint8_t>(kMaxTimestamps);
        std::vector<uint64_t> tsShader = std::vector<uint64_t>(kMaxTimestamps); // pixel shader of a draw interval (category 0)
        uint32_t tsCount = 0;
        bool pending = false; // submitted, not waited for yet
    };
    inline FrameSlot g_slots[kFramesInFlight];
    constexpr VkBufferUsageFlags kUniformPoolUsage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    constexpr VkBufferUsageFlags kUploadPoolUsage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    inline uint32_t g_slot = 0;
    inline VkDeviceSize g_uploadsPeak = 0, g_uniformsPeak = 0;
    void WaitSlot(FrameSlot& s);
    void WaitForGpu();
    void EvictUnusedImages(); // texture cache section
    void BeginFrame();
    void EndFrame(bool wait = false);
    void MarkGpu(uint8_t category, bool frameBoundary);
    VkFormat ColorRtFormat(xenos::ColorRenderTargetFormat f);
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
        uint64_t writeSerial = 0; // bumped by anything that writes the image (draws, clears, tile transfers): deferred resolve copies
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
        bool postSkipped = false; // the last draw into it was a skipped post-effect producer (post_effects.cpp)
    };
    inline std::map<std::tuple<uint32_t, VkFormat, uint32_t, uint32_t>, std::unique_ptr<RenderTarget>> g_rts;
    inline RenderTarget* g_tileOwner[xenos::kEdramTileCount] = {};
    // Cleared EDRAM tiles: `by` (the tile's owner) last wrote it with a single clear value. When
    // another render target claims such a tile (PES clears through a 4x-MSAA alias of a target,
    // then draws into the 1x one), the tile is filled with that value inside the claimant's render
    // pass instead of being copied (a transfer between passes; it splits passes on the iPad).
    // Any other write to the tile forgets the value. PES_NO_CLEAR_FILL=1 / "clearfill off": copy (A/B).
    struct TileClear { RenderTarget* by = nullptr; VkClearValue value{}; };
    inline TileClear g_tileClear[xenos::kEdramTileCount] = {};
    // Lazy clears: a known clear draw over whole tiles is not drawn at all; its tiles are marked
    // pending (g_tileClear holds the value, the owner's image does not have it yet). The next
    // render pass of the owner or of a target taking the tiles fills them inside the pass
    // (AcquireTiles); anything else reading the owner's image first applies them
    // (FlushPendingClears). PES_NO_LAZY_CLEAR=1 / PES_CONTROL "lazyclear off": draw clears (A/B).
    inline bool g_tilePending[xenos::kEdramTileCount] = {};
    inline bool g_noLazyClear = std::getenv("PES_NO_LAZY_CLEAR") != nullptr;
    inline uint64_t g_lazyClears = 0, g_pendingFlushes = 0; // stats (reset every 120 frames)
    inline bool g_lazyClearUsed = false; // lazy clears happened (stats)
    inline uint32_t g_pendingTileCount = 0; // tiles with g_tilePending set: the checks run only when > 0
    inline void SetTilePending(uint32_t e, bool pending)
    {
        if (g_tilePending[e] == pending) return;
        g_tilePending[e] = pending;
        if (pending) ++g_pendingTileCount; else --g_pendingTileCount;
    }
    void FlushPendingClears(RenderTarget* t);
    struct TileFill { VkRect2D rect; VkClearValue value; };
    inline bool g_noClearFill = std::getenv("PES_NO_CLEAR_FILL") != nullptr;
    inline uint64_t g_depthClearDrawsKnown = 0;
    inline uint64_t g_tilesFilled = 0, g_clearDraws = 0, g_clearDrawsKnown = 0; // stats: clear draws examined / with a constant color
    inline uint64_t g_tileTransfers = 0;
    inline uint64_t g_rtsEvicted = 0;
    inline std::map<std::tuple<VkFormat, VkFormat, bool, bool>, VkRenderPass> g_renderPasses; // (color, depth, load color, load depth)
    inline std::map<std::tuple<RenderTarget*, RenderTarget*, uint32_t, uint32_t>, VkFramebuffer> g_framebuffers; // (color, depth, width, height)
    // Pass areas (BeginPass): the n-th pass on the same attachments in a frame -> the area it needed.
    struct PassArea { VkExtent2D extent{}; uint64_t frame = 0; };
    inline std::map<std::tuple<RenderTarget*, RenderTarget*, uint32_t>, PassArea> g_passAreaHistory;
    inline std::map<std::pair<RenderTarget*, RenderTarget*>, uint32_t> g_passSlots; // passes begun this frame per attachments
    inline uint64_t g_passSlotFrame = ~0ull;
    inline uint32_t g_passSlot = 0; // the current pass's n
    inline bool g_fullPasses = std::getenv("PES_PASS_FULL") != nullptr; // whole-image passes (A/B; PES_CONTROL "passes full/area")
    // PES_CONTROL "passlog <frames>": print every render pass of the next frames (attachments, area,
    // draws with their pixel shaders, end reason) - to see the pass structure of a scene.
    inline bool g_noPlainFetch = std::getenv("PES_NO_PLAIN_FETCH") != nullptr; // PES_CONTROL "plainfetch on|off"
    inline bool g_noDepthKeep = std::getenv("PES_NO_DEPTH_KEEP") != nullptr; // see DrawImpl, before BeginPass
    inline uint64_t g_depthKeptDraws = 0; // stats: no-depth draws that stayed in a pass with depth
    inline uint32_t g_passLogFrames = 0;
    inline bool g_passLogNow = false;
    inline std::string g_passLogDraws;
    inline bool g_noDiscardLoads = std::getenv("PES_NO_DISCARD_LOADS") != nullptr; // see BeginPass
    inline uint64_t g_discardedLoadPixels = 0; // attachment pixels not loaded (stats)
    constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; // also on Apple GPUs (no D24S8 there)
    VkRenderPass RenderPassFor(VkFormat color, VkFormat depth, bool loadColor = true, bool loadDepth = true);
    void TileSize(xenos::MsaaSamples msaa, bool is64bpp, uint32_t& w, uint32_t& h);
    bool Is64bpp(VkFormat f);
    void ClearRenderTarget(Image& img, bool isDepth);
    RenderTarget* GetRenderTarget(uint32_t base, VkFormat format, uint32_t pitchTiles, xenos::MsaaSamples msaa);
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
    inline std::vector<Retired> g_retired;
    inline uint64_t g_rtGrows = 0;
    inline uint64_t g_stagingOneOff = 0, g_stagingOneOffBytes = 0;
    uint8_t* StagingAllocate(VkDeviceSize size, VkBuffer& buffer, VkDeviceSize& offset);
    void EnsureRows(RenderTarget* rt, uint32_t rows);
    struct DepthToColorState
    {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkDescriptorPool pools[kFramesInFlight] = {}; // one per frame slot
        VkSampler sampler = VK_NULL_HANDLE;
        bool failed = false;
    };
    inline DepthToColorState g_d2c;
    void ResetDepthToColorPool();
    bool InitDepthToColor();
    VkImageView AspectView(RenderTarget* rt, VkImageAspectFlags aspect);
    void DepthToColor(RenderTarget* from, RenderTarget* to, const std::vector<uint32_t>& tiles, uint32_t tileW, uint32_t tileH);
    void CaptureNote(const char* fmt, ...); // a line in the capture step log (if capturing)
    struct Image;
    void DumpImage(Image& img, uint32_t width, uint32_t height, const std::string& path, bool swapRedBlue);
    // Makes rt the owner of the EDRAM tiles under the rectangle (rt pixels), first copying in
    // the content of tiles another render target owns (if the formats allow it).
    // Tile transfer regions -> fewer, larger rectangles: tiles adjacent in both images (same pitch,
    // the usual case) merge into row spans, and identical spans of consecutive rows into blocks.
    struct TileRect { int32_t sx, sy, sw, sh, dx, dy, dw, dh; };
    void MergeTileRects(std::vector<TileRect>& v);
    void MergeRegions(std::vector<VkImageCopy>& v);
    void MergeRegions(std::vector<VkImageBlit>& v);
    void MarkTilesCleared(RenderTarget* rt, const VkRect2D& area, const VkClearValue& value, bool known = true);
    bool CanLazyClear(const RenderTarget* t, const VkRect2D& area);
    bool TryLazyClear(RenderTarget* t, const VkRect2D& area, const VkClearValue& value);
    // fills (draws only): cleared tiles of another target (g_tileClear) are not copied but listed
    // here, to be cleared inside the draw's render pass. writes: the caller writes the area (forgets
    // the tiles' clear values; a resolve only reads).
    inline uint64_t g_tilesSkipped = 0;
    void AcquireTiles(RenderTarget* rt, int32_t x, int32_t y, uint32_t w, uint32_t h, const VkRect2D* noCopy = nullptr, std::vector<TileFill>* fills = nullptr, bool writes = false);
    void EndPass(const char* why);
    VkExtent2D PassExtent(const RenderTarget* color, const RenderTarget* depth);
    void BeginPass(RenderTarget* color, RenderTarget* depth, const VkRect2D* need = nullptr, bool overwritesColor = false, bool overwritesDepth = false);
    void FlushPendingClears(RenderTarget* t);
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
        uint64_t sampledGen = 0;       // generation a texture fetch last used (see SkipUnsampledResolve)
        uint32_t unsampledResolves = 0; // resolves in a row whose result nothing sampled
        bool skippedContent = false;   // the last resolve's copy was skipped: the image is stale
        // Skipped resolves, replayed if the surface is sampled while their source is unchanged.
        struct PendingResolve { RenderTarget* rt; uint64_t serial; int32_t sx, sy, dx, dy; uint32_t w, h; };
        std::vector<PendingResolve> pendingResolves;
        bool mirror = false;           // this frame's resolves into it are stored to guest memory
        bool postSkipped = false;      // the last resolve came from a skipped post-effect producer
        Image img;
        // Copies of the top-left fetch-sized region (see SurfaceForFetchSize), by width << 16 | height.
        struct Crop { Image img; uint64_t generation = 0; uint64_t lastUsedFrame = 0; };
        std::map<uint32_t, Crop> crops;
    };
    inline std::vector<std::unique_ptr<Surface>> g_surfaces;
    inline uint64_t g_surfaceEpoch = 0; // bumped when a surface is created or retired (address index below)
    Surface* FindSurfaceContaining(uint32_t address, uint32_t pitch, uint32_t bytesPerPixel, VkFormat format);
    Surface* GetResolveSurface(uint32_t destBase, uint32_t destPitch, uint32_t destHeight, uint32_t bpp, VkFormat format);
    void ResolveDestOffset(const Surface* s, uint32_t destBase, const draw_util::ResolveInfo& info, int32_t& dstX, int32_t& dstY);
    Surface* FindSurfaceAt(uint32_t address);
    // Resolves keep their result on the GPU only; guest memory under a surface keeps whatever it
    // held. When the game reuses that memory (loads a skin/kit texture there), the surface is
    // stale. A sample of the guest bytes is kept at every resolve; if the CPU changed them, the
    // surface is retired and the texture is read from guest memory again.
    constexpr uint32_t kSnapshotSamples = 16, kSnapshotBytes = 64;
    inline uint64_t g_surfaceGeneration = 0;
    void SnapshotSurface(Surface* s);
    // Resolves whose result is never sampled (PES resolves shadow-map color, depth of passes it
    // never reads, ...: about half of the resolved pixels in heavy scenes). Each skipped copy also
    // keeps the render pass open (no store + reload on the tile-based iPad GPU). A surface is
    // skipped after kSkipAfter resolves in a row that nothing sampled; small ones never (the CPU
    // reads kits / numbers from guest memory). Call before SnapshotSurface. If a skipped surface is
    // sampled after all, copies resume at once (MarkSurfaceSampled; at most that one use is stale).
    // PES_NO_RESOLVE_SKIP=1: always copy (A/B).
    inline bool g_noResolveSkip = std::getenv("PES_NO_RESOLVE_SKIP") != nullptr; // PES_CONTROL "resolveskip on/off"
    struct ResolveSkipStats { uint64_t resolves = 0, skipped = 0, skippedPixels = 0, staleSamples = 0, replayed = 0, lost = 0; };
    inline ResolveSkipStats g_resolveSkip;
    void CopyColorResolve(RenderTarget* rt, Surface* s, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h);
    bool CopyDepthResolve(RenderTarget* d, Surface* s, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h);
    void DeferResolve(Surface* s, RenderTarget* rt, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h);
    bool SkipUnsampledResolve(Surface* s, uint32_t w, uint32_t h);
    void MarkSurfaceSampled(Surface* s);
    bool SurfaceStillInMemory(Surface* s);
    inline uint64_t g_surfacesRetired = 0;
    inline uint64_t g_surfacesEvicted = 0;
    // The surface a texture fetch reads, if any: a resolve result at that address whose texel
    // layout fits the fetch (no block-compressed formats; depth formats <-> depth surfaces) and
    // whose memory the CPU hasn't overwritten since.
    // PES_STRICT_SURFACE_EXTENT=<seconds> (diagnostic, from ChatGPT's gpt-surface-extent.patch):
    // from that time on, a surface smaller than the fetch is not used for it (a small resolve at
    // an address later reused by a bigger texture). Switched on in Present (same-run A/B).
    inline bool g_strictExtent = false;
    inline uint64_t g_extentRejects = 0;
    Surface* FindSurfaceForTexture(uint32_t base, const xenos::xe_gpu_texture_fetch_t& fetch);
    // ------------------------------------------------------------------ guest textures
    struct HostFormat
    {
        VkFormat format;
        uint32_t swizzle; // host swizzle applied before the guest swizzle
    };
    // BC (DXT1/3/5) textures are sampled directly when the device supports BC; otherwise (some
    // Apple GPU / iPadOS combinations) they are decoded to RGBA8 on the CPU at upload.
    // PES_FORCE_BC_DECODE=1 forces the decode path (to test it on a PC). Set in Renderer::Init.
    inline bool g_bcDecode = false;
    bool IsBC(xenos::TextureFormat f);
    void DecodeBC1Color(const uint8_t* b, uint8_t* rgba, bool fourColor);
    void DecodeBCBlock(xenos::TextureFormat f, const uint8_t* b, uint8_t* rgba);
    bool GetHostFormat(xenos::TextureFormat f, HostFormat& out);
    uint32_t CombineSwizzle(uint32_t guestSwizzle, uint32_t hostSwizzle);
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
    inline std::map<std::vector<uint32_t>, std::unique_ptr<Texture>> g_textures;
    inline Image g_null2D, g_null3D, g_nullCube;
    void SwapDwords(uint8_t* data, size_t bytes, xenos::Endian endian);
    Texture* GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch, const HostFormat& host);
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
    inline std::map<std::array<uint32_t, 5>, std::unique_ptr<ComposedTexture>> g_composed; // base, width, height, pitch, tiled
    inline uint64_t g_composedBuilds = 0, g_composedUses = 0;
    void EvictUnusedImages();
    ComposedTexture* ComposeFromSurfaces(const xenos::xe_gpu_texture_fetch_t& fetch, uint32_t base, bool& memoryExact);
    VkImageView TextureView(uint32_t fetchIndex, xenos::FetchOpDimension dimension);
    // ------------------------------------------------------------------ samplers
    inline std::map<uint64_t, VkSampler> g_samplers;
    VkSamplerAddressMode AddressMode(xenos::ClampMode m);
    VkSampler GetSampler(const SpirvShader::SamplerBinding& binding);
    // ------------------------------------------------------------------ shaders
    struct ShaderEntry
    {
        std::unique_ptr<SpirvShader> shader;
        std::unordered_map<uint64_t, VkShaderModule> modules; // by modification
    };
    inline std::unordered_map<uint64_t, ShaderEntry> g_shaderCache;
    inline ShaderEntry* g_current[2] = {};
    VkShaderModule GetModule(ShaderEntry& e, uint64_t modification);
    // ------------------------------------------------------------------ layouts + pipelines
    inline VkDescriptorSetLayout g_layoutShared = VK_NULL_HANDLE, g_layoutConstants = VK_NULL_HANDLE, g_layoutEmpty = VK_NULL_HANDLE;
    inline VkDescriptorPool g_persistentPool = VK_NULL_HANDLE;
    inline VkDescriptorSet g_sharedSet = VK_NULL_HANDLE;
    inline std::map<std::pair<uint32_t, uint32_t>, VkDescriptorSetLayout> g_textureLayouts[2]; // [vertex, pixel] by (textures, samplers)
    struct PipelineLayout
    {
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkDescriptorSetLayout textures[2] = {};
    };
    inline std::map<std::vector<uint32_t>, PipelineLayout> g_pipelineLayouts;
    inline std::map<std::vector<uint64_t>, VkPipeline> g_pipelines;
    // Driver pipeline cache, persisted in userdata/vulkan_pipeline_cache.bin so the (slow)
    // compilation of translated shaders happens once, not on every launch.
    inline VkPipelineCache g_pipelineCache = VK_NULL_HANDLE;
    inline bool g_pipelineCacheDirty = false;
    inline const char* kPipelineCachePath = "userdata/vulkan_pipeline_cache.bin";
    void SavePipelineCache();
    VkDescriptorSetLayout TextureLayout(bool pixel, uint32_t textures, uint32_t samplers);
    PipelineLayout* GetPipelineLayout(uint32_t tv, uint32_t sv, uint32_t tp, uint32_t sp);
    VkBlendFactor BlendFactor(uint32_t f);
    VkBlendOp BlendOp(uint32_t op);
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
    VkPipeline GetPipeline(const PipelineDesc& d);
    bool HostTopology(xenos::PrimitiveType p, VkPrimitiveTopology& topology, bool& convert);
    void FillSystemConstants(Translator::SystemConstants& sc, bool polygonal, const draw_util::ViewportInfo& vi, uint32_t usedTextureMask);
    VkDescriptorBufferInfo UploadUniform(const void* data, size_t size);
    VkDescriptorBufferInfo UploadFloatConstants(const Shader* shader, uint32_t baseRegister);
    VkDescriptorSet ConstantsSet(const VkDescriptorBufferInfo* buffers, uint32_t* dynamicOffsets);
    std::vector<uint8_t> PackFloatConstants(const Shader& shader, uint32_t baseRegister);
    VkDescriptorSet TextureSet(VkDescriptorSetLayout layout, const SpirvShader* shader);
    // ------------------------------------------------------------------ per-draw capture
    // PES_CAPTURE=<seconds>: records the first whole frame after that time. After every draw and
    // resolve, a thumbnail of the color render target goes into an atlas (12 x 45 slots of
    // 160x90, the target's top-left 1280x720 pixels scaled down) and a line into a step log:
    // userdata/capture/atlas.ppm + steps.txt (slot numbers are row-major in the atlas).
    constexpr uint32_t kThumbW = 160, kThumbH = 90, kThumbCols = 12, kThumbRows = 45;
    inline Image g_capAtlas;
    enum class CaptureState { Off, Armed, Active, Done };
    inline CaptureState g_capState = CaptureState::Off;
    inline FILE* g_capLog = nullptr;
    inline std::string g_capDir = "userdata/capture";
    inline std::deque<std::string> g_controlCapture; // PES_CONTROL "capture <name>" queue: consecutive frames, one each
    inline std::string g_controlCaptureAck;   // name of the running control capture (acknowledged when written)
    inline bool g_capArmed = false, g_capArmHit = false; // "capture-on-resolve": keep only a frame with a matching resolve
    inline uint32_t g_capArmMin = 0, g_capArmHitAddr = 0, g_capArmW = 128, g_capArmH = 128;
    inline bool g_controlDumpSurfaces = false; // PES_CONTROL "surfaces": dump small surfaces + composed textures
    inline bool g_controlDumpAllSurfaces = false; // "surfaces all": every RGBA8 surface, any size
    void CaptureNote(const char* fmt, ...);
    inline uint32_t g_capStep = 0, g_capSlots = 0;
    struct CaptureProbe { uint32_t step; bool pre = false; int32_t color = -1, depth = -1, stencil = -1; VkFormat colorFormat = VK_FORMAT_UNDEFINED; };
    inline std::vector<CaptureProbe> g_capProbes;
    inline Buffer g_capProbeBuf;
    inline uint32_t g_capProbeUsed = 0;
    inline RenderTarget* g_capDepthRt = nullptr; // depth target of the current draw (probe)
    void CaptureProbeNow(RenderTarget* color, RenderTarget* depth, bool pre);
    inline Image* g_capThumbImage = nullptr; // thumbnail this image instead of the step target (resolve destination)
    void CaptureStep(RenderTarget* rt, const char* fmt, ...);
    VkRect2D EdramRect(const draw_util::ResolveEdramInfo& e, uint32_t originalBase, const draw_util::ResolveInfo& info, VkExtent2D limit);
    void ClearDepth(const draw_util::ResolveInfo& info);
    // Depth copy (resolve of the depth buffer to a k_24_8 / k_24_8_FLOAT texture, e.g. shadow maps).
    // The surface holds depth as R32_SFLOAT in 0..1, which is what the guest's fetch of a 24-bit
    // depth texture returns in .x. Vulkan can't copy depth to a color image directly: go through a
    // buffer.
    inline Buffer g_depthCopyBuf;
    inline int g_abPhase = 0; // 0 off, 1 frame A, 2 frame B, 3 done
    bool CopyDepthResolve(RenderTarget* d, Surface* s, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h);
    void ResolveDepth(const draw_util::ResolveInfo& info);
    // CPU view of resolves. Mirrored resolve memory holds the texels in the order our GPU path
    // reads back correctly ([c3 c0 c1 c2] for swap+8in32 resolves). PES's CPU code that consumes
    // resolve output (XG untile sub_82B1A9D0, then an ARGB->RGBA byte rotation and the kit
    // compositor) expects the EDRAM channel order as-is: with the GPU order a jersey number
    // tile's transparent background (EDRAM 00 F0 F0 F0) came out opaque cyan on every in-play
    // shirt (2026-11-07). So when the XG untile reads a mirrored resolve, that memory is
    // rewritten in raw EDRAM order first. Registry: renderer thread adds, guest threads consume.
    struct MirroredResolve { uint32_t size; bool rbSwapped; xenos::Endian endian; bool fromLargeSource; };
    inline std::mutex g_mirroredMutex;
    inline std::map<uint32_t, MirroredResolve> g_mirrored; // physical base -> layout of the stored texels
    inline uint64_t g_cpuViewConversions = 0;
    // Stores a 32bpp surface into guest memory as the console's resolve would: Xenos tiled layout
    // (pitch = surface pitch), dest endianness, red/blue as the resolve left them in memory. The
    // stored bytes are exactly what GetTexture turns back into the surface's texels. Synchronous
    // GPU readback (experiment, see PES_RESOLVE_WRITEBACK).
    inline uint64_t g_writeBacks = 0;
    void WriteBackSurface(Surface* s, xenos::Endian128 endian, bool fromLargeSource = false);
    void CopyColorResolve(RenderTarget* rt, Surface* s, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h);
    void Resolve();
    // ------------------------------------------------------------------ draw
    void DrawImpl2(uint32_t indexBase, uint32_t indexSizeDword);
    void DrawImpl(uint32_t indexBase, uint32_t indexSizeDword);
    void DrawImpl2(uint32_t indexBase, uint32_t indexSizeDword);
    // ------------------------------------------------------------------ debug readback
    inline uint64_t g_dumpSurfacesFrame = ~0ull;
    void DumpImage(Image& img, uint32_t width, uint32_t height, const std::string& path, bool swapRedBlue = false);
    void DumpAllSurfaces();
    bool CreateNullImage(Image& img, VkImageType type, VkImageViewType viewType, uint32_t layers);
    // ------------------------------------------------------------------ optional post effects (post_effects.cpp)
    enum class PostEffect : uint32_t { Glow, PauseBlur, Soft, Count };
    // Since start, never reset (Gpu's "slow batch" line attributes its time with these).
    inline uint64_t g_compileUsTotal = 0, g_pipelinesTotal = 0, g_modulesTotal = 0;
    inline uint32_t g_postOff = 0;          // bit per PostEffect: turned off (launcher "Blur effects", PES_POST_OFF, control "post")
    inline uint64_t g_postDrawPs = 0;       // the current draw's pixel shader (skipped-surface diagnostics)
    inline uint64_t g_postSkippedDraws[uint32_t(PostEffect::Count)] = {};
    const char* PostEffectName(PostEffect e);
    bool SkipPostEffectDraw(uint64_t psHash, RenderTarget* rt); // true: don't issue this draw
    void NotePostSkippedSample(const Surface* s);
    uint32_t ParsePostEffects(const char* list); // "glow,pause,soft" / "all" -> mask
}
