// Debug capture (PES_CAPTURE / control "capture"): per-draw thumbnails and step log.
// Part of the renderer (see renderer_internal.h for the file map).
#include "renderer_internal.h"

namespace RendererImpl
{
    void CaptureNote(const char* fmt, ...)
    {
        if (g_capState != CaptureState::Active || !g_capLog) return;
        va_list args;
        va_start(args, fmt);
        std::vfprintf(g_capLog, fmt, args);
        va_end(args);
        std::fputc('\n', g_capLog);
    }

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

    // Reads an RGBA8 image back and writes it as a binary PPM (debugging).
    void DumpImage(Image& img, uint32_t width, uint32_t height, const std::string& path, bool swapRedBlue){
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
