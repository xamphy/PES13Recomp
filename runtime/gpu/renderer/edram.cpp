// EDRAM model: render targets, tile ownership, depth->color transfers, lazy clears, render passes.
// Part of the renderer (see renderer_internal.h for the file map).
#include "renderer_internal.h"

namespace RendererImpl
{
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

    // Render pass with an optional color attachment (0) and an optional depth/stencil attachment.
    // loadColor / loadDepth false: the pass starts by overwriting the whole attachment (DONT_CARE).
    VkRenderPass RenderPassFor(VkFormat color, VkFormat depth, bool loadColor, bool loadDepth){
        auto it = g_renderPasses.find({ color, depth, loadColor, loadDepth });
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
            a[count].loadOp = loadColor ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
            a[count].loadOp = a[count].stencilLoadOp = loadDepth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
        g_renderPasses.emplace(std::make_tuple(color, depth, loadColor, loadDepth), pass);
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
        PassEndContext passEndContext("clear");
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
        ++rt->writeSerial; // a new image (content copied over)
        // Grow by at least half (few regrowths), whole tile rows.
        uint32_t height = std::max(rows, rt->img.height + rt->img.height / 2);
        height = std::min(rt->fullHeight, (height + rt->tileH - 1) / rt->tileH * rt->tileH);

        EndPass("target grows");
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
            if (std::get<0>(fb->first) == rt || std::get<1>(fb->first) == rt) { r.framebuffers.push_back(fb->second); fb = g_framebuffers.erase(fb); }
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
#include "../shaders/depth_to_color.spv.h"
        ;

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
        PassEndContext passEndContext("depth to color");
        if (tiles.empty() || !InitDepthToColor()) return;
        FlushPendingClears(from); // lazy clears (normally applied by AcquireTiles already)
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
    // After `rt` wrote `value` over `area` (a clear): whole tiles inside it remember the value,
    // tiles it only partly covers forget theirs.
    void MarkTilesCleared(RenderTarget* rt, const VkRect2D& area, const VkClearValue& value, bool known){
        if (!rt || !area.extent.width || !area.extent.height || area.offset.x < 0 || area.offset.y < 0) return;
        const int32_t ax1 = area.offset.x + int32_t(area.extent.width), ay1 = area.offset.y + int32_t(area.extent.height);
        const uint32_t c0 = uint32_t(area.offset.x) / rt->tileW, r0 = uint32_t(area.offset.y) / rt->tileH;
        const uint32_t c1 = std::min(uint32_t(ax1 - 1) / rt->tileW, rt->pitchTiles - 1), r1 = uint32_t(ay1 - 1) / rt->tileH;
        for (uint32_t r = r0; r <= r1; ++r)
            for (uint32_t c = c0; c <= c1; ++c)
            {
                const uint32_t local = r * rt->pitchTiles + c;
                if (local >= xenos::kEdramTileCount) return;
                const uint32_t e = (rt->base + local) & (xenos::kEdramTileCount - 1);
                const int32_t tx = int32_t(c * rt->tileW), ty = int32_t(r * rt->tileH);
                const bool whole = tx >= area.offset.x && ty >= area.offset.y && tx + int32_t(rt->tileW) <= ax1 && ty + int32_t(rt->tileH) <= ay1;
                if (whole && known && g_tileOwner[e] == rt) g_tileClear[e] = { rt, value };
                else g_tileClear[e].by = nullptr;
            }
    }

    // Lazy clear of `area` (whole tiles of t, already owned by t): the tiles remember the value as
    // pending instead of being cleared now (see g_tilePending). False (nothing done) when lazy
    // clears are off or the area is not made of whole tiles.
    bool CanLazyClear(const RenderTarget* t, const VkRect2D& area)
    {
        if (!t || g_noLazyClear || g_noClearFill || !area.extent.width || !area.extent.height || area.offset.x < 0 || area.offset.y < 0) return false;
        if (area.offset.x % int32_t(t->tileW) || area.offset.y % int32_t(t->tileH) || area.extent.width % t->tileW || area.extent.height % t->tileH)
            return false;
        const uint32_t c0 = uint32_t(area.offset.x) / t->tileW, r0 = uint32_t(area.offset.y) / t->tileH;
        const uint32_t c1 = (uint32_t(area.offset.x) + area.extent.width - 1) / t->tileW;
        const uint32_t r1 = (uint32_t(area.offset.y) + area.extent.height - 1) / t->tileH;
        if (c1 >= t->pitchTiles || (r1 + 1) * t->tileH > t->img.height) return false;
        for (uint32_t r = r0; r <= r1; ++r)
            for (uint32_t c = c0; c <= c1; ++c)
            {
                const uint32_t local = r * t->pitchTiles + c;
                if (local >= xenos::kEdramTileCount) return false;
                if (g_tileOwner[(t->base + local) & (xenos::kEdramTileCount - 1)] != t) return false;
            }
        return true;
    }

    bool TryLazyClear(RenderTarget* t, const VkRect2D& area, const VkClearValue& value)
    {
        if (!CanLazyClear(t, area)) return false;
        const uint32_t c0 = uint32_t(area.offset.x) / t->tileW, r0 = uint32_t(area.offset.y) / t->tileH;
        const uint32_t c1 = (uint32_t(area.offset.x) + area.extent.width - 1) / t->tileW;
        const uint32_t r1 = (uint32_t(area.offset.y) + area.extent.height - 1) / t->tileH;
        MarkTilesCleared(t, area, value);
        for (uint32_t r = r0; r <= r1; ++r)
            for (uint32_t c = c0; c <= c1; ++c)
                SetTilePending((t->base + r * t->pitchTiles + c) & (xenos::kEdramTileCount - 1), true);
        ++t->writeSerial;
        ++g_lazyClears;
        g_lazyClearUsed = true;
        return true;
    }

    void AcquireTiles(RenderTarget* rt, int32_t x, int32_t y, uint32_t w, uint32_t h, const VkRect2D* noCopy,
        std::vector<TileFill>* fills, bool writes){
        PassEndContext passEndContext("EDRAM tile transfer");
        if (!rt || !w || !h || x < 0 || y < 0) return;
        rt->maxRow = std::max(rt->maxRow, uint32_t(y) + h);
        const uint32_t c0 = uint32_t(x) / rt->tileW, r0 = uint32_t(y) / rt->tileH;
        const uint32_t c1 = std::min((uint32_t(x) + w - 1) / rt->tileW, rt->pitchTiles - 1);
        const uint32_t r1 = std::min((uint32_t(y) + h - 1) / rt->tileH, rt->fullHeight / rt->tileH - 1);
        EnsureRows(rt, (r1 + 1) * rt->tileH);
        // Lazy clears: pending tiles that will not be filled inside the coming pass (or
        // overwritten whole) must first be applied to their owner's image, which is read below.
        if (g_pendingTileCount)
        {
            std::vector<RenderTarget*> flush;
            for (uint32_t r = r0; r <= r1; ++r)
                for (uint32_t c = c0; c <= c1; ++c)
                {
                    const uint32_t local = r * rt->pitchTiles + c;
                    if (local >= xenos::kEdramTileCount) break;
                    const uint32_t e = (rt->base + local) & (xenos::kEdramTileCount - 1);
                    RenderTarget* o = g_tileOwner[e];
                    if (!o || !g_tilePending[e]) continue;
                    const int32_t tx = int32_t(c * rt->tileW), ty = int32_t(r * rt->tileH);
                    const bool inImage = uint32_t(tx) + rt->tileW <= rt->img.width && uint32_t(ty) + rt->tileH <= rt->img.height;
                    const bool overwritten = noCopy && tx >= noCopy->offset.x && ty >= noCopy->offset.y &&
                        tx + int32_t(rt->tileW) <= noCopy->offset.x + int32_t(noCopy->extent.width) &&
                        ty + int32_t(rt->tileH) <= noCopy->offset.y + int32_t(noCopy->extent.height);
                    const bool handled = overwritten || (o == rt ? fills && inImage
                        : fills && !g_noClearFill && inImage && o->format == rt->format && o->isDepth == rt->isDepth);
                    if (!handled && std::find(flush.begin(), flush.end(), o) == flush.end()) flush.push_back(o);
                }
            for (RenderTarget* t : flush) FlushPendingClears(t);
        }
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
                TileClear& tc = g_tileClear[e];
                const bool cleared = o && tc.by == o;
                // A pending tile (lazy clear) of rt itself: filled inside the coming pass. Pending
                // tiles of other owners are filled below, overwritten whole, or were flushed above.
                if (g_tilePending[e])
                {
                    SetTilePending(e, false);
                    if (o == rt && cleared && fills)
                    {
                        const int32_t dx = int32_t(c * rt->tileW), dy = int32_t(r * rt->tileH);
                        if (uint32_t(dx) + rt->tileW <= rt->img.width && uint32_t(dy) + rt->tileH <= rt->img.height)
                        {
                            fills->push_back({ { { dx, dy }, { rt->tileW, rt->tileH } }, tc.value });
                            ++g_tilesFilled;
                        }
                    }
                }
                if (writes || (cleared && o != rt && !fills)) tc.by = nullptr; // written / copied below: value forgotten
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
                if (fills && cleared && !g_noClearFill && o->format == rt->format && o->isDepth == rt->isDepth)
                {
                    const int32_t dx = int32_t(c * rt->tileW), dy = int32_t(r * rt->tileH);
                    if (uint32_t(dx) + rt->tileW <= rt->img.width && uint32_t(dy) + rt->tileH <= rt->img.height)
                    {
                        fills->push_back({ { { dx, dy }, { rt->tileW, rt->tileH } }, tc.value });
                        if (!writes) tc.by = rt; // rt will hold the same value
                        ++g_tilesFilled;
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
        const bool transfers = !d2c.empty() || !pending.empty();
        if (transfers) MarkGpu(3), ++rt->writeSerial;
        struct TransferMark { bool on; ~TransferMark() { if (on) MarkGpu(4); } } transferMark{ transfers };
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

    void EndPass(const char* why)
    {
        if (!g_inPass) return;
        PassStats& s = g_passStats;
        ++s.endReasons[g_passEndContext ? g_passEndContext : why];
        ++s.byDraws[g_passDraws == 0 ? 0 : g_passDraws == 1 ? 1 : g_passDraws <= 4 ? 2 : 3];
        s.draws += g_passDraws;
        s.framebufferPixels += uint64_t(g_passExtent.width) * g_passExtent.height;
        if (g_passDraws && g_passX1 > g_passX0 && g_passY1 > g_passY0)
            s.touchedPixels += uint64_t(std::min<int32_t>(g_passX1, int32_t(g_passExtent.width)) - g_passX0) *
                uint64_t(std::min<int32_t>(g_passY1, int32_t(g_passExtent.height)) - g_passY0);
        vkCmdEndRenderPass(g_cmd);
        if (g_passLogNow)
        {
            std::printf("[passlog] f%llu color %s depth %s | %ux%u (need %ux%u) | %u draws:%s | end: %s\n", (unsigned long long)g_frame,
                g_passColor ? (std::to_string(g_passColor->base) + "/" + std::to_string(int(g_passColor->format)) + "x" + std::to_string(uint32_t(g_passColor->msaa))).c_str() : "-",
                g_passDepth ? std::to_string(g_passDepth->base).c_str() : "-",
                g_passExtent.width, g_passExtent.height, g_passNeed.width, g_passNeed.height, g_passDraws, g_passLogDraws.c_str(),
                g_passEndContext ? g_passEndContext : why);
            g_passLogDraws.clear();
        }
        // Area this pass needed (predicts the same pass next frame, see BeginPass).
        {
            PassArea& h = g_passAreaHistory[{ g_passColor, g_passDepth, g_passSlot }];
            if (h.frame != g_frame) h = { g_passNeed, g_frame };
            else h.extent = { std::max(h.extent.width, g_passNeed.width), std::max(h.extent.height, g_passNeed.height) };
        }
        const RenderTarget* endedColor = g_passColor;
        const RenderTarget* endedDepth = g_passDepth;
        g_inPass = false;
        g_passColor = g_passDepth = nullptr;
        MarkGpu(0); // the pass (its draws)
        // GPU time of this pass by label ([passgpu]): from the mark before BeginPass to this one.
        if (!g_noPassGpu && g_passSpans && g_passBeginTs != UINT32_MAX && g_tsCount > 0 && g_tsCount - 1 > g_passBeginTs)
        {
            // Label id by a numeric key; the text is built once per kind of pass (it was ~1 ms CPU
            // per frame when formatted for every pass).
            uint64_t key = uint64_t(endedColor ? (uint32_t(endedColor->format) << 4 | uint32_t(endedColor->msaa)) : 0xFFFF) |
                uint64_t(endedDepth != nullptr) << 16 | uint64_t(g_passExtent.width) << 20 | uint64_t(g_passExtent.height) << 40;
            for (uint64_t h : g_passPs) key = key * 0x9E3779B97F4A7C15ull ^ h;
            auto [it, added] = g_passLabelIds.try_emplace(key, uint32_t(g_passLabels.size()));
            if (added)
            {
                char label[160];
                const std::string fmt = endedColor ? " fmt" + std::to_string(int(endedColor->format)) + " msaa" + std::to_string(uint32_t(endedColor->msaa)) : "";
                int n = std::snprintf(label, sizeof(label), "%s%ux%u%s%s ps", endedColor ? "" : "depth-only ", g_passExtent.width, g_passExtent.height,
                    fmt.c_str(), endedDepth ? " +depth" : "");
                for (uint64_t h : g_passPs) if (h && n < int(sizeof(label)) - 8) n += std::snprintf(label + n, sizeof(label) - n, " %04llx", (unsigned long long)(h & 0xFFFF));
                if (!g_passPs[0]) std::snprintf(label + n, sizeof(label) - n, " -");
                g_passLabels.push_back(label);
            }
            g_passSpans->push_back({ g_passBeginTs, g_tsCount - 1, it->second, g_passDraws });
        }
        g_passBeginTs = UINT32_MAX;
    }

    // Framebuffer size: the smaller of the attachments.
    VkExtent2D PassExtent(const RenderTarget* color, const RenderTarget* depth)
    {
        VkExtent2D e{ kRtWidth, kRtMaxHeight };
        for (const RenderTarget* rt : { color, depth })
            if (rt) e = { std::min(e.width, rt->img.width), std::min(e.height, rt->img.height) };
        return e;
    }

    // Pass area: a pass loads and stores its whole framebuffer (on the iPad's tile-based GPU that is
    // memory traffic for every pixel of it), so the framebuffer covers only what the pass draws:
    // from the origin to the bottom-right of its draws / clears (`need`), rounded up to 32 pixels.
    // Pixels outside it are not touched (Vulkan keeps them), so this is exact. The same pass last
    // frame (same attachments, n-th such pass in the frame) predicts its area; if a later draw
    // needs more, the pass ends and restarts bigger (its content is stored and loaded again).
    // PES_PASS_FULL=1: whole image every time (A/B).
    // overwritesColor / overwritesDepth: the first draw writes every pixel of `need` (a clear rectangle,
    // see DrawImpl). A new pass that covers exactly that area then skips loading the old content
    // (DONT_CARE) - on the tile-based iPad GPU that load is memory traffic for the whole area.
    // PES_NO_DISCARD_LOADS=1 / PES_CONTROL "discardloads off": always load (A/B).
    void BeginPass(RenderTarget* color, RenderTarget* depth, const VkRect2D* need, bool overwritesColor, bool overwritesDepth){
        const bool fullPasses = g_fullPasses;
        const VkExtent2D full = PassExtent(color, depth);
        uint32_t nx = full.width, ny = full.height;
        if (need && !fullPasses)
        {
            nx = std::min<uint32_t>(full.width, uint32_t(std::max<int32_t>(need->offset.x + int32_t(need->extent.width), 1)));
            ny = std::min<uint32_t>(full.height, uint32_t(std::max<int32_t>(need->offset.y + int32_t(need->extent.height), 1)));
        }
        uint32_t slot;
        VkExtent2D needed{ nx, ny }; // what the draws need (the history records this, not the prediction)
        bool exact = false; // framebuffer = exactly `need` (no rounding), old content not loaded
        if (g_inPass && g_passColor == color && g_passDepth == depth)
        {
            g_passNeed = { std::max(g_passNeed.width, nx), std::max(g_passNeed.height, ny) };
            if (nx <= g_passExtent.width && ny <= g_passExtent.height) return;
            needed = g_passNeed;
            nx = std::max(nx, g_passExtent.width);
            ny = std::max(ny, g_passExtent.height);
            slot = g_passSlot; // the same pass, grown (history keeps the larger area)
            EndPass("pass grows");
        }
        else
        {
            EndPass("target change");
            if (g_passSlotFrame != g_frame) { g_passSlots.clear(); g_passSlotFrame = g_frame; }
            slot = g_passSlots[{ color, depth }]++;
            const auto h = g_passAreaHistory.find({ color, depth, slot });
            if (h != g_passAreaHistory.end() && !fullPasses)
            {
                nx = std::max(nx, h->second.extent.width);
                ny = std::max(ny, h->second.extent.height);
            }
            exact = (overwritesColor || overwritesDepth) && need && !fullPasses && !g_noDiscardLoads &&
                need->offset.x == 0 && need->offset.y == 0 && nx == needed.width && ny == needed.height;
        }
        auto roundUp = [](uint32_t v, uint32_t limit) { return std::min(limit, (v + 31) & ~31u); };
        const VkExtent2D extent = exact ? needed : VkExtent2D{ roundUp(nx, full.width), roundUp(ny, full.height) };
        if (exact) g_discardedLoadPixels += uint64_t(extent.width) * extent.height * ((overwritesColor && color) + (overwritesDepth && depth));
        if (color) Transition(color->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        if (depth) Transition(depth->img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        const VkRenderPass pass = RenderPassFor(color ? color->format : VK_FORMAT_UNDEFINED, depth ? depth->format : VK_FORMAT_UNDEFINED,
            !(exact && overwritesColor), !(exact && overwritesDepth));
        g_passSlot = slot;
        VkFramebuffer& fb = g_framebuffers[{ color, depth, extent.width, extent.height }];
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
        MarkGpu(3); // work since the previous mark (layout transitions etc.)
        g_passBeginTs = g_tsCount ? g_tsCount - 1 : UINT32_MAX;
        g_passPs[0] = g_passPs[1] = g_passPs[2] = 0;
        vkCmdBeginRenderPass(g_cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        g_inPass = true;
        g_passColor = color;
        g_passDepth = depth;
        ++g_passBegins;
        g_passDraws = 0;
        g_passExtent = extent;
        g_passNeed = needed;
        g_passX0 = g_passY0 = INT32_MAX;
        g_passX1 = g_passY1 = 0;
    }

    // Writes t's pending lazy clears into its image (a render pass with clear calls only), before
    // something reads the image other than a render pass of t that fills them (AcquireTiles).
    void FlushPendingClears(RenderTarget* t)
    {
        if (!t || !g_pendingTileCount) return;
        std::vector<std::pair<VkClearValue, std::vector<VkClearRect>>> groups;
        int32_t x1 = 0, y1 = 0;
        const uint32_t rows = t->img.height / t->tileH;
        for (uint32_t r = 0; r < rows; ++r)
            for (uint32_t c = 0; c < t->pitchTiles; ++c)
            {
                const uint32_t local = r * t->pitchTiles + c;
                if (local >= xenos::kEdramTileCount) break;
                const uint32_t e = (t->base + local) & (xenos::kEdramTileCount - 1);
                if (!g_tilePending[e] || g_tileOwner[e] != t) continue;
                SetTilePending(e, false);
                const int32_t dx = int32_t(c * t->tileW), dy = int32_t(r * t->tileH);
                if (uint32_t(dx) + t->tileW > t->img.width) continue;
                const VkClearValue& v = g_tileClear[e].value;
                auto g = std::find_if(groups.begin(), groups.end(), [&](const auto& x) { return !std::memcmp(&x.first, &v, sizeof(VkClearValue)); });
                if (g == groups.end()) { groups.push_back({ v, {} }); g = groups.end() - 1; }
                VkClearRect* last = g->second.empty() ? nullptr : &g->second.back();
                if (last && last->rect.offset.y == dy && last->rect.offset.x + int32_t(last->rect.extent.width) == dx)
                    last->rect.extent.width += t->tileW;
                else
                    g->second.push_back({ { { dx, dy }, { t->tileW, t->tileH } }, 0, 1 });
                x1 = std::max(x1, dx + int32_t(t->tileW));
                y1 = std::max(y1, dy + int32_t(t->tileH));
            }
        if (groups.empty()) return;
        ++g_pendingFlushes;
        const VkRect2D need{ { 0, 0 }, { uint32_t(x1), uint32_t(y1) } };
        PassEndContext passEndContext("pending clears");
        BeginPass(t->isDepth ? nullptr : t, t->isDepth ? t : nullptr, &need);
        for (const auto& [value, rects] : groups)
        {
            VkClearAttachment att{};
            att.aspectMask = t->isDepth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
            att.colorAttachment = 0;
            att.clearValue = value;
            vkCmdClearAttachments(g_cmd, 1, &att, uint32_t(rects.size()), rects.data());
        }
        ++t->writeSerial;
        EndPass("pending clears");
    }
}
