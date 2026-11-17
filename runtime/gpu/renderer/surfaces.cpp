// Resolve surfaces: guest-address ranges that resolves copy into and textures sample.
// Part of the renderer (see renderer_internal.h for the file map).
#include "renderer_internal.h"

namespace RendererImpl
{
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

    // Record a skipped copy (same destination rectangle: the newer one replaces it).
    void DeferResolve(Surface* s, RenderTarget* rt, int32_t sx, int32_t sy, int32_t dx, int32_t dy, uint32_t w, uint32_t h)
    {
        auto& v = s->pendingResolves;
        std::erase_if(v, [&](const Surface::PendingResolve& p) { return p.dx == dx && p.dy == dy && p.w == w && p.h == h; });
        if (v.size() >= 16) v.erase(v.begin());
        v.push_back({ rt, rt->writeSerial, sx, sy, dx, dy, w, h });
    }

    bool SkipUnsampledResolve(Surface* s, uint32_t w, uint32_t h)
    {
        const bool off = g_noResolveSkip;
        constexpr uint32_t kSkipAfter = 120;
        ++g_resolveSkip.resolves;
        if (s->generation && s->sampledGen != s->generation) ++s->unsampledResolves;
        else s->unsampledResolves = 0;
        const bool skip = !off && s->unsampledResolves >= kSkipAfter && (s->pitch > 256 || s->height > 256);
        s->skippedContent = skip;
        if (skip)
        {
            ++g_resolveSkip.skipped;
            g_resolveSkip.skippedPixels += uint64_t(w) * h;
        }
        return skip;
    }

    void MarkSurfaceSampled(Surface* s)
    {
        s->lastSampledFrame = g_frame;
        if (s->skippedContent)
        {
            // Replay the skipped copies whose source is still unchanged (oldest first).
            uint32_t replayed = 0, lost = 0;
            for (const auto& p : s->pendingResolves)
            {
                const bool alive = std::any_of(g_rts.begin(), g_rts.end(), [&](const auto& kv) { return kv.second.get() == p.rt; });
                if (!alive || p.rt->writeSerial != p.serial) { ++lost; continue; }
                if (s->isDepth ? CopyDepthResolve(p.rt, s, p.sx, p.sy, p.dx, p.dy, p.w, p.h)
                               : (CopyColorResolve(p.rt, s, p.sx, p.sy, p.dx, p.dy, p.w, p.h), true))
                    ++replayed;
                else
                    ++lost;
            }
            s->pendingResolves.clear();
            if (replayed) s->generation = ++g_surfaceGeneration; // crops / composed textures refresh
            g_resolveSkip.replayed += replayed;
            g_resolveSkip.lost += lost;
            if (lost) ++g_resolveSkip.staleSamples;
            static uint32_t logs = 0;
            if (logs++ < 20)
                std::printf("[render] skipped resolve surface 0x%08X %ux%u sampled after %u unsampled resolves: %u copies replayed, %u lost (stale) - copying again\n",
                    s->base, s->pitch, s->height, s->unsampledResolves, replayed, lost);
            s->skippedContent = false;
            s->unsampledResolves = 0;
        }
        s->sampledGen = s->generation;
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
}
