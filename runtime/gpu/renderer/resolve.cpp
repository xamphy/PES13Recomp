// Resolves (EDRAM -> surfaces): color/depth copies, depth clears, write-back to guest memory.
// Part of the renderer (see renderer_internal.h for the file map).
#include "renderer_internal.h"

namespace RendererImpl
{
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
        AcquireTiles(depth, rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height, &rect); // whole tiles inside are overwritten
        const uint32_t v = info.rb_depth_clear;
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        clear.clearValue.depthStencil.depth = info.depth_edram_info.format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8)
            ? xenos::Float20e4To32(v >> 8) : xenos::UNorm24To32(v >> 8);
        clear.clearValue.depthStencil.stencil = v & 0xFF;
        const VkClearRect clearRect{ rect, 0, 1 };
        if (TryLazyClear(depth, rect, clear.clearValue)) return; // filled by the next pass using the tiles
        BeginPass(nullptr, depth, &rect);
        ++depth->writeSerial;
        vkCmdClearAttachments(g_cmd, 1, &clear, 1, &clearRect);
        MarkTilesCleared(depth, rect, clear.clearValue);
    }

    // Depth -> R32_SFLOAT surface: through a buffer (depth/stencil images cannot be copied to color images).
    bool CopyDepthResolve(RenderTarget* d, Surface* s, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h)
    {
        FlushPendingClears(d); // lazy clears (normally applied by AcquireTiles already)
        const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
        if (g_depthCopyBuf.size < bytes)
        {
            if (g_depthCopyBuf.buffer) WarnOnce("depth copy larger than the staging buffer");
            if (!g_depthCopyBuf.buffer && !CreateDeviceBuffer(g_depthCopyBuf, 2048ull * 2048 * 4,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT))
                return false;
            if (g_depthCopyBuf.size < bytes) return false;
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
        c.imageOffset = { sx, sy, 0 };
        c.imageExtent = { w, h, 1 };
        vkCmdCopyImageToBuffer(g_cmd, d->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_depthCopyBuf.buffer, 1, &c);
        bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &bb, 0, nullptr);
        c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        c.imageOffset = { dx, dy, 0 };
        vkCmdCopyBufferToImage(g_cmd, g_depthCopyBuf.buffer, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        return true;
    }

    void ResolveDepth(const draw_util::ResolveInfo& info)
    {
        PassEndContext passEndContext("resolve depth");
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
        const bool skipCopy = SkipUnsampledResolve(s, src.extent.width, src.extent.height);
        SnapshotSurface(s);
        int32_t dstX, dstY;
        ResolveDestOffset(s, info.copy_dest_base, info, dstX, dstY);
        if (dstX < 0 || dstY < 0 || uint32_t(dstX) >= s->img.width || uint32_t(dstY) >= s->img.height) return;
        const uint32_t w = std::min<uint32_t>(src.extent.width, s->img.width - dstX);
        const uint32_t h = std::min<uint32_t>(src.extent.height, s->img.height - dstY);
        if (skipCopy)
        {
            DeferResolve(s, d, src.offset.x, src.offset.y, dstX, dstY, w, h);
            CaptureStep(nullptr, "DEPTH RESOLVE (skipped: never sampled) -> 0x%08X (surface 0x%08X)", info.copy_dest_base, s->base);
            return;
        }
        s->pendingResolves.clear();
        if (g_abPhase == 2)
        {
            // A/B frame B: depth textures read 0 (as before depth copies existed).
            Transition(s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue zero{};
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(g_cmd, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        }
        else if (!CopyDepthResolve(d, s, src.offset.x, src.offset.y, dstX, dstY, w, h))
            return;
        static uint32_t logs = 0;
        if (logs++ < 6)
            std::printf("[render] depth copy: depth base %u pitch %u msaa %u rect %d,%d %ux%u -> 0x%08X (surface 0x%08X) at %d,%d, dest fmt %u\n",
                d->base, d->pitchTiles, uint32_t(d->msaa), src.offset.x, src.offset.y, w, h, info.copy_dest_base, s->base, dstX, dstY,
                uint32_t(info.copy_dest_info.copy_dest_format));
        CaptureStep(nullptr, "DEPTH RESOLVE depth base %u pitch %u msaa %u rect %d,%d %ux%u -> 0x%08X (surface 0x%08X) at %d,%d dest fmt %u%s",
            d->base, d->pitchTiles, uint32_t(d->msaa), src.offset.x, src.offset.y, w, h, info.copy_dest_base, s->base, dstX, dstY,
            uint32_t(info.copy_dest_info.copy_dest_format), info.IsClearingDepth() ? " +depthclear" : "");
    }

    void WriteBackSurface(Surface* s, xenos::Endian128 endian, bool fromLargeSource){
        PassEndContext passEndContext("write-back to guest memory");
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

    void CopyColorResolve(RenderTarget* rt, Surface* s, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h)
    {
        FlushPendingClears(rt); // lazy clears (normally applied by AcquireTiles already)
        Transition(rt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        Transition(s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy copy{};
        copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.srcOffset = { sx, sy, 0 };
        copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.dstOffset = { dx, dy, 0 };
        copy.extent = { w, h, 1 };
        vkCmdCopyImage(g_cmd, rt->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->img.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }

    void Resolve()
    {
        PassEndContext passEndContext("resolve");
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
        s->postSkipped = rt->postSkipped; // diagnostics for optional post effects (post_effects.cpp)
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
        const bool skipCopy = SkipUnsampledResolve(s, width, height);
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
        if (skipCopy && width && height && dstX >= 0 && dstY >= 0)
            DeferResolve(s, rt, srcX, srcY, dstX, dstY, width, height); // before the clear below changes the target
        if (width && height && dstX >= 0 && dstY >= 0 && !skipCopy)
        {
            s->pendingResolves.clear();
            CopyColorResolve(rt, s, srcX, srcY, dstX, dstY, width, height);
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
                if (!TryLazyClear(rt, rect.rect, clear.clearValue)) // lazy: filled by the next pass using the tiles
                {
                BeginPass(rt, nullptr, &rect.rect);
                ++rt->writeSerial;
                vkCmdClearAttachments(g_cmd, 1, &clear, 1, &rect);
                MarkTilesCleared(rt, rect.rect, clear.clearValue);
                }
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
}
