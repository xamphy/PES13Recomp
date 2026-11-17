// Textures: formats, BC decode, guest texture upload and cache, composed textures, samplers.
// Part of the renderer (see renderer_internal.h for the file map).
#include "renderer_internal.h"

namespace RendererImpl
{
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

    // Uploads a 2D texture from guest memory with its mip levels (records the copies). Without the
    // mips every texture was sampled at full resolution even far away: the pitch and other
    // detailed textures shimmered like TV static in motion (2026-11-14). The levels are read from
    // fetch.mip_address in the guest layout (texture_util), the smallest ones from the packed mip
    // tail (GetPackedMipOffset), as Xenia does. PES_NO_MIPS=1: base level only (A/B).
    Texture* GetTexture(const xenos::xe_gpu_texture_fetch_t& fetch, const HostFormat& host)
    {
        PassEndContext passEndContext("texture upload");
        const uint32_t width = fetch.size_2d.width + 1, height = fetch.size_2d.height + 1;
        const uint32_t base = fetch.base_address << 12;
        const uint32_t mipBase = fetch.mip_address << 12;
        static const bool noMips = std::getenv("PES_NO_MIPS") != nullptr;
        uint32_t levels = 1;
        if (!noMips && mipBase && fetch.mip_max_level >= 1)
        {
            uint32_t maxLevel = 0;
            while ((std::max(width, height) >> (maxLevel + 1)) >= 1) ++maxLevel; // down to the 1x1 level
            levels = std::min<uint32_t>(fetch.mip_max_level, maxLevel) + 1;
        }
        std::vector<uint32_t> key = { base, uint32_t(fetch.format), width, height, fetch.tiled, fetch.pitch, uint32_t(fetch.endianness),
            levels > 1 ? mipBase : 0, levels, fetch.packed_mips };
        auto& slot = g_textures[key];
        if (!slot)
        {
            if (width == 512 && height == 256 && !fetch.tiled && fetch.format == xenos::TextureFormat::k_8_8_8_8 && base >= 0x18000000u)
                std::printf("[render] new 512x256 kit texture 0x%08X (frame %llu)\n", base, (unsigned long long)g_frame);
            slot = std::make_unique<Texture>();
            if (!CreateImage(slot->img, host.format, width, height, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1, 1, levels))
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
            fetch.tiled, fetch.format, fetch.packed_mips, true, levels - 1);
        const uint32_t blockW = info->block_width, blockH = info->block_height, bpb = info->bytes_per_block();
        const uint32_t srcBytes = layout.base.level_data_extent_bytes;
        if (!srcBytes || base + srcBytes > kSharedMemorySize) return tex;
        uint32_t mipBytes = levels > 1 ? layout.mips_total_extent_bytes : 0;
        if (mipBytes && mipBase + mipBytes > kSharedMemorySize) mipBytes = 0;

        // PES_TEX_HASH=1: hash every frame (no write tracking), for A/B.
        static const bool texHashOnly = std::getenv("PES_TEX_HASH") != nullptr;
        if (!texHashOnly && tex->watched && tex->uploadedFrame != ~0ull && !MemoryWatch::WrittenSince(base, srcBytes, tex->watchEpoch) &&
            (!mipBytes || !MemoryWatch::WrittenSince(mipBase, mipBytes, tex->watchEpoch)))
        {
            tex->uploadedFrame = g_frame;
            return tex;
        }
        // Epoch, then protect, then read the bytes (a write in between is recorded as newer).
        const uint32_t epoch = MemoryWatch::Epoch();
        const bool watch = !texHashOnly && tex->changedStreak < 3;
        if (watch)
        {
            MemoryWatch::Protect(base, srcBytes);
            if (mipBytes) MemoryWatch::Protect(mipBase, mipBytes);
        }
        tex->watched = watch;
        tex->watchEpoch = epoch;
        // Skip the upload if the guest bytes did not change since the last one.
        const auto hashStart = std::chrono::steady_clock::now();
        uint64_t hash = XXH3_64bits(g_xeMemory.TranslatePhysical(base), srcBytes);
        if (mipBytes) hash ^= XXH3_64bits(g_xeMemory.TranslatePhysical(mipBase), mipBytes) * 0x9E3779B97F4A7C15ull;
        g_texHashUs += std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - hashStart).count();
        g_texHashBytes += srcBytes + mipBytes;
        if (tex->uploadedFrame != ~0ull && hash == tex->contentHash)
        {
            tex->uploadedFrame = g_frame;
            tex->changedStreak = 0;
            return tex;
        }
        if (tex->uploadedFrame != ~0ull) ++tex->changedStreak;
        tex->contentHash = hash;
        ++g_time.textureUploads;

        uint32_t bpbLog2 = 0;
        while ((1u << bpbLog2) < bpb) ++bpbLog2;
        const bool decode = g_bcDecode && IsBC(fetch.format);
        static std::vector<uint8_t> src, blocks;
        std::vector<VkBufferImageCopy> copies;
        std::vector<VkBuffer> buffers; // the staging buffer of each copy
        for (uint32_t level = 0; level < levels; ++level)
        {
            if (level && !mipBytes) break;
            // Where this level's blocks are: the base, a mip level, or a sub-rectangle of the packed tail.
            const uint32_t guestLevel = level ? std::min(level, layout.packed_level) : 0;
            const auto& l = level ? layout.mips[guestLevel] : layout.base;
            const uint32_t from = level ? mipBase + layout.mip_offsets_bytes[guestLevel] : base;
            const uint32_t bytes = l.level_data_extent_bytes;
            if (!bytes || from + bytes > kSharedMemorySize) break;
            uint32_t ox = 0, oy = 0, oz = 0;
            if (level && level >= layout.packed_level)
                texture_util::GetPackedMipOffset(width, height, 1, fetch.format, level, ox, oy, oz);
            const uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
            const uint32_t blocksX = (lw + blockW - 1) / blockW, blocksY = (lh + blockH - 1) / blockH;
            src.resize(bytes);
            std::memcpy(src.data(), g_xeMemory.TranslatePhysical(from), bytes);
            SwapDwords(src.data(), src.size(), fetch.endianness);
            uint8_t* dst = nullptr;
            VkDeviceSize offset = 0;
            VkBuffer staging = VK_NULL_HANDLE;
            if (decode)
            {
                blocks.assign(size_t(blocksX) * blocksY * bpb, 0);
                dst = blocks.data();
            }
            else if (!(dst = StagingAllocate(VkDeviceSize(blocksX) * blocksY * bpb, staging, offset)))
                break;
            const uint32_t pitchBlocks = l.row_pitch_bytes / bpb; // tiled: aligned to 32 blocks
            for (uint32_t by = 0; by < blocksY; ++by)
                for (uint32_t bx = 0; bx < blocksX; ++bx)
                {
                    const uint32_t sx = bx + ox, sy = by + oy;
                    const uint32_t s = fetch.tiled ? uint32_t(texture_address::Tiled2D(int32_t(sx), int32_t(sy), pitchBlocks, bpbLog2))
                                                   : sy * l.row_pitch_bytes + sx * bpb;
                    if (s + bpb <= bytes)
                        std::memcpy(dst + (size_t(by) * blocksX + bx) * bpb, src.data() + s, bpb);
                }
            VkBufferImageCopy copy{};
            if (decode)
            {
                // Blocks -> RGBA8 rows of blocksX * 4 pixels; the copy takes the level's lw x lh of them.
                const uint32_t rowPixels = blocksX * 4;
                uint8_t* rgba = StagingAllocate(VkDeviceSize(rowPixels) * blocksY * 4 * 4, staging, offset);
                if (!rgba) break;
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
            else
            {
                copy.bufferRowLength = blocksX * blockW;
                copy.bufferImageHeight = blocksY * blockH;
            }
            copy.bufferOffset = offset;
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1 };
            copy.imageExtent = { lw, lh, 1 }; // the whole level (allowed for block formats when not a block multiple)
            copies.push_back(copy);
            buffers.push_back(staging);
        }
        if (copies.empty()) return tex;
        Transition(tex->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        MarkGpu(3);
        for (size_t i = 0; i < copies.size(); ++i)
            vkCmdCopyBufferToImage(g_cmd, buffers[i], tex->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copies[i]);
        MarkGpu(2);
        tex->uploadedFrame = g_frame;
        return tex;
    }

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
                if (std::get<0>(fb->first) == rt || std::get<1>(fb->first) == rt) { vkDestroyFramebuffer(g_dev, fb->second, nullptr); fb = g_framebuffers.erase(fb); }
                else ++fb;
            std::erase_if(g_passAreaHistory, [&](const auto& kv) { return std::get<0>(kv.first) == rt || std::get<1>(kv.first) == rt; });
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
        for (Surface* s : parts) MarkSurfaceSampled(s);
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

        EndPass("composed texture");
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
        PassEndContext passEndContext("texture sampled (RT/surface)");
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
            MarkSurfaceSampled(s);
            if (s->postSkipped) NotePostSkippedSample(s);
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
        // Mip filter and level range (GetTexture uploads the mip levels), anisotropic filtering as
        // the game asks for it. PES_NO_MIPS=1: base level only (A/B).
        static const bool noMips = std::getenv("PES_NO_MIPS") != nullptr;
        const auto mip = noMips ? xenos::TextureFilter::kBaseMap : pick(binding.mip_filter, fetch.mip_filter);
        const auto aniso = binding.aniso_filter == xenos::AnisoFilter::kUseFetchConst ? fetch.aniso_filter : binding.aniso_filter;
        const uint32_t minLevel = fetch.mip_min_level, maxLevel = std::max<uint32_t>(fetch.mip_max_level, minLevel);
        const uint64_t key = uint64_t(mag) | (uint64_t(min) << 2) | (uint64_t(cx) << 4) | (uint64_t(cy) << 7) |
            (uint64_t(cz) << 10) | (uint64_t(border) << 13) | (uint64_t(mip) << 16) | (uint64_t(aniso) << 18) |
            (uint64_t(minLevel) << 21) | (uint64_t(maxLevel) << 25);
        auto it = g_samplers.find(key);
        if (it != g_samplers.end()) return it->second;
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = mag == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.minFilter = min == xenos::TextureFilter::kLinear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = mip == xenos::TextureFilter::kLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = AddressMode(cx);
        si.addressModeV = AddressMode(cy);
        si.addressModeW = AddressMode(cz);
        if (mip == xenos::TextureFilter::kBaseMap)
            si.maxLod = 0.25f; // base level only
        else
        {
            si.minLod = float(minLevel);
            si.maxLod = float(maxLevel); // images have fewer levels when the game stores fewer (clamped by Vulkan)
        }
        if (aniso >= xenos::AnisoFilter::kMax_2_1 && aniso <= xenos::AnisoFilter::kMax_16_1 && g_vk->enabledFeatures.samplerAnisotropy)
        {
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy = std::min(float(1u << (uint32_t(aniso) - 1)), g_vk->properties.limits.maxSamplerAnisotropy);
        }
        si.borderColor = border == uint32_t(xenos::BorderColor::k_ABGR_White) ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                                               : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        VkSampler s = VK_NULL_HANDLE;
        vkCreateSampler(g_dev, &si, nullptr, &s);
        g_samplers.emplace(key, s);
        return s;
    }
}
