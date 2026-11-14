// Vulkan presenter. Deliberately shader-free for now: the frame is uploaded to an
// image and blitted onto the swap chain image (vkCmdBlitImage handles scaling and the
// RGBA -> BGRA conversion).
// The window itself is platform code behind window.h (window_win32.cpp / window_sdl.cpp).
#include "presenter.h"
#include "vulkan_context.h"
#include "window.h"

#include <volk.h>

#ifdef __APPLE__
#   include <dlfcn.h>
#endif

#include <algorithm>
#include <cmath>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace
{
    VkInstance g_instance = VK_NULL_HANDLE;
    VkSurfaceKHR g_surface = VK_NULL_HANDLE;
    VkPhysicalDevice g_physical = VK_NULL_HANDLE;
    VkDevice g_device = VK_NULL_HANDLE;
    uint32_t g_queueFamily = 0;
    VkQueue g_queue = VK_NULL_HANDLE;

    VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
    VkFormat g_swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D g_swapExtent{};
    std::vector<VkImage> g_swapImages;

    VkCommandPool g_cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer g_cmd = VK_NULL_HANDLE;
    VkFence g_fence = VK_NULL_HANDLE;
    VkSemaphore g_acquired = VK_NULL_HANDLE;
    std::vector<VkSemaphore> g_rendered; // one per swap chain image

    // Upload resources (recreated when the frame size changes)
    uint32_t g_uploadWidth = 0, g_uploadHeight = 0;
    VkBuffer g_staging = VK_NULL_HANDLE;
    VkDeviceMemory g_stagingMemory = VK_NULL_HANDLE;
    void* g_stagingPtr = nullptr;
    VkImage g_frame = VK_NULL_HANDLE;
    VkDeviceMemory g_frameMemory = VK_NULL_HANDLE;

    bool g_ready = false;
    std::mutex g_presentMutex;
    Video::VulkanContext g_context;

    bool Check(VkResult r, const char* what)
    {
        if (r != VK_SUCCESS)
            std::fprintf(stderr, "[video] %s failed: %d\n", what, int(r));
        return r == VK_SUCCESS;
    }

    uint32_t FindMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props)
    {
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_physical, &mp);
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
            if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
                return i;
        return UINT32_MAX;
    }

    // ------------------------------------------------------------------ swap chain
    void DestroySwapchain()
    {
        for (VkSemaphore s : g_rendered) vkDestroySemaphore(g_device, s, nullptr);
        g_rendered.clear();
        if (g_swapchain) vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
        g_swapchain = VK_NULL_HANDLE;
        g_swapImages.clear();
    }

    bool CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR caps;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_physical, g_surface, &caps);
        if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
            return false; // minimized

        uint32_t count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &count, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(g_physical, g_surface, &count, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (const auto& f : formats)
            if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                chosen = f;

        g_swapFormat = chosen.format;
        g_swapExtent = caps.currentExtent;

        VkSwapchainCreateInfoKHR ci{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
        ci.surface = g_surface;
        ci.minImageCount = std::min(caps.minImageCount + 1, caps.maxImageCount ? caps.maxImageCount : 8u);
        ci.imageFormat = chosen.format;
        ci.imageColorSpace = chosen.colorSpace;
        ci.imageExtent = g_swapExtent;
        ci.imageArrayLayers = 1;
        ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.preTransform = caps.currentTransform;
        ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        ci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // vsync: also paces the game to the display
        ci.clipped = VK_TRUE;
        if (!Check(vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain), "vkCreateSwapchainKHR"))
            return false;

        vkGetSwapchainImagesKHR(g_device, g_swapchain, &count, nullptr);
        g_swapImages.resize(count);
        vkGetSwapchainImagesKHR(g_device, g_swapchain, &count, g_swapImages.data());
        g_rendered.resize(count);
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        for (auto& s : g_rendered) vkCreateSemaphore(g_device, &si, nullptr, &s);
        return true;
    }

    // ------------------------------------------------------------------ upload
    void DestroyUpload()
    {
        if (g_stagingPtr) vkUnmapMemory(g_device, g_stagingMemory);
        if (g_staging) vkDestroyBuffer(g_device, g_staging, nullptr);
        if (g_stagingMemory) vkFreeMemory(g_device, g_stagingMemory, nullptr);
        if (g_frame) vkDestroyImage(g_device, g_frame, nullptr);
        if (g_frameMemory) vkFreeMemory(g_device, g_frameMemory, nullptr);
        g_stagingPtr = nullptr; g_staging = VK_NULL_HANDLE; g_stagingMemory = VK_NULL_HANDLE;
        g_frame = VK_NULL_HANDLE; g_frameMemory = VK_NULL_HANDLE;
        g_uploadWidth = g_uploadHeight = 0;
    }

    bool CreateUpload(uint32_t width, uint32_t height)
    {
        DestroyUpload();
        const VkDeviceSize size = VkDeviceSize(width) * height * 4;

        VkBufferCreateInfo bi{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bi.size = size;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (!Check(vkCreateBuffer(g_device, &bi, nullptr, &g_staging), "vkCreateBuffer")) return false;
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_device, g_staging, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!Check(vkAllocateMemory(g_device, &ai, nullptr, &g_stagingMemory), "vkAllocateMemory (staging)")) return false;
        vkBindBufferMemory(g_device, g_staging, g_stagingMemory, 0);
        vkMapMemory(g_device, g_stagingMemory, 0, size, 0, &g_stagingPtr);

        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = { width, height, 1 };
        ii.mipLevels = 1;
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!Check(vkCreateImage(g_device, &ii, nullptr, &g_frame), "vkCreateImage")) return false;
        vkGetImageMemoryRequirements(g_device, g_frame, &req);
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!Check(vkAllocateMemory(g_device, &ai, nullptr, &g_frameMemory), "vkAllocateMemory (frame)")) return false;
        vkBindImageMemory(g_device, g_frame, g_frameMemory, 0);

        g_uploadWidth = width;
        g_uploadHeight = height;
        return true;
    }

    void Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
        VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage)
    {
        VkImageMemoryBarrier b{ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = from;
        b.newLayout = to;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdPipelineBarrier(g_cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // ------------------------------------------------------------------ upscaling (FSR 1 / SGSR 1)
    // The game image is copied into `in` (RGBA, sampled), upscaled by compute shaders to the
    // on-screen size - FSR 1: EASU into `mid`, RCAS sharpening into `out`; SGSR 1: one pass into
    // `out` - and `out` is copied 1:1 into the swap chain image. Off: the plain linear blit.
    const uint32_t kFsrEasuSpv[] =
#include "shaders/fsr_easu.spv.h"
    ;
    const uint32_t kFsrRcasSpv[] =
#include "shaders/fsr_rcas.spv.h"
    ;
    const uint32_t kSgsr1Spv[] =
#include "shaders/sgsr1.spv.h"
    ;

    std::atomic<int> g_upscaler{ [] {
        const char* e = std::getenv("PES_UPSCALER");
        if (!e) return 0;
        if (!std::strcmp(e, "fsr") || !std::strcmp(e, "fsr1")) return int(Video::Upscaler::Fsr1);
        if (!std::strcmp(e, "sgsr") || !std::strcmp(e, "sgsr1")) return int(Video::Upscaler::Sgsr1);
        return 0;
    }() };

    std::atomic<uint32_t> g_upscaleHeight{ [] { const char* e = std::getenv("PES_UPSCALE_HEIGHT"); return e ? uint32_t(std::atoi(e)) : 0u; }() };

    struct UpscaleImage { VkImage image = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE; };
    struct Upscale
    {
        bool initialized = false, usable = false;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        VkPipeline easu = VK_NULL_HANDLE, rcas = VK_NULL_HANDLE, sgsr = VK_NULL_HANDLE;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet setEasu = VK_NULL_HANDLE, setRcas = VK_NULL_HANDLE, setSgsr = VK_NULL_HANDLE;
        UpscaleImage in, mid, out;
        uint32_t inW = 0, inH = 0, outW = 0, outH = 0;
        // GPU time of the upscale (two timestamps per frame), averaged over 120 frames for the log.
        VkQueryPool queries = VK_NULL_HANDLE;
        bool queryPending = false;
        double gpuMs = 0;
        uint32_t frames = 0;
    } g_up;

    VkPipeline CreateComputePipeline(const uint32_t* code, size_t size)
    {
        VkShaderModuleCreateInfo mi{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        mi.codeSize = size;
        mi.pCode = code;
        VkShaderModule module = VK_NULL_HANDLE;
        if (!Check(vkCreateShaderModule(g_device, &mi, nullptr, &module), "vkCreateShaderModule (upscaler)")) return VK_NULL_HANDLE;
        VkComputePipelineCreateInfo ci{ VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = g_up.layout;
        VkPipeline pipeline = VK_NULL_HANDLE;
        Check(vkCreateComputePipelines(g_device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline), "vkCreateComputePipelines (upscaler)");
        vkDestroyShaderModule(g_device, module, nullptr);
        return pipeline;
    }

    // Pipelines, sampler, descriptor sets (once). False if the device cannot run the upscalers.
    bool InitUpscale()
    {
        if (g_up.initialized) return g_up.usable;
        g_up.initialized = true;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(g_physical, VK_FORMAT_R8G8B8A8_UNORM, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
        if ((fp.optimalTilingFeatures & need) != need)
        {
            std::fprintf(stderr, "[video] upscaling unavailable: RGBA8 storage images not supported - plain scaling\n");
            return false;
        }
        VkSamplerCreateInfo si{ VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(g_device, &si, nullptr, &g_up.sampler);
        const VkDescriptorSetLayoutBinding bindings[2] = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
        };
        VkDescriptorSetLayoutCreateInfo li{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        li.bindingCount = 2;
        li.pBindings = bindings;
        vkCreateDescriptorSetLayout(g_device, &li, nullptr, &g_up.setLayout);
        const VkPushConstantRange push{ VK_SHADER_STAGE_COMPUTE_BIT, 0, 80 };
        VkPipelineLayoutCreateInfo pli{ VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &g_up.setLayout;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &push;
        vkCreatePipelineLayout(g_device, &pli, nullptr, &g_up.layout);
        g_up.easu = CreateComputePipeline(kFsrEasuSpv, sizeof(kFsrEasuSpv));
        g_up.rcas = CreateComputePipeline(kFsrRcasSpv, sizeof(kFsrRcasSpv));
        g_up.sgsr = CreateComputePipeline(kSgsr1Spv, sizeof(kSgsr1Spv));
        const VkDescriptorPoolSize sizes[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3 } };
        VkDescriptorPoolCreateInfo dpi{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = 3;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        vkCreateDescriptorPool(g_device, &dpi, nullptr, &g_up.pool);
        const VkDescriptorSetLayout layouts[3] = { g_up.setLayout, g_up.setLayout, g_up.setLayout };
        VkDescriptorSetAllocateInfo dai{ VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = g_up.pool;
        dai.descriptorSetCount = 3;
        dai.pSetLayouts = layouts;
        VkDescriptorSet sets[3] = {};
        vkAllocateDescriptorSets(g_device, &dai, sets);
        g_up.setEasu = sets[0];
        g_up.setRcas = sets[1];
        g_up.setSgsr = sets[2];
        VkQueryPoolCreateInfo qi{ VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2;
        vkCreateQueryPool(g_device, &qi, nullptr, &g_up.queries);
        g_up.usable = g_up.easu && g_up.rcas && g_up.sgsr && g_up.setSgsr;
        if (!g_up.usable)
            std::fprintf(stderr, "[video] upscaling unavailable: pipeline creation failed - plain scaling\n");
        return g_up.usable;
    }

    void DestroyUpscaleImage(UpscaleImage& i)
    {
        if (i.view) vkDestroyImageView(g_device, i.view, nullptr);
        if (i.image) vkDestroyImage(g_device, i.image, nullptr);
        if (i.memory) vkFreeMemory(g_device, i.memory, nullptr);
        i = {};
    }

    bool CreateUpscaleImage(UpscaleImage& i, uint32_t w, uint32_t h, VkImageUsageFlags usage)
    {
        DestroyUpscaleImage(i);
        VkImageCreateInfo ii{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = { w, h, 1 };
        ii.mipLevels = ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = usage;
        if (!Check(vkCreateImage(g_device, &ii, nullptr, &i.image), "vkCreateImage (upscaler)")) return false;
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(g_device, i.image, &req);
        VkMemoryAllocateInfo ai{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!Check(vkAllocateMemory(g_device, &ai, nullptr, &i.memory), "vkAllocateMemory (upscaler)")) return false;
        vkBindImageMemory(g_device, i.image, i.memory, 0);
        VkImageViewCreateInfo vi{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = i.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_UNORM;
        vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        return Check(vkCreateImageView(g_device, &vi, nullptr, &i.view), "vkCreateImageView (upscaler)");
    }

    void WriteUpscaleSet(VkDescriptorSet set, const UpscaleImage& input, VkImageLayout inputLayout, const UpscaleImage& output)
    {
        const VkDescriptorImageInfo src{ g_up.sampler, input.view, inputLayout };
        const VkDescriptorImageInfo dst{ VK_NULL_HANDLE, output.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET }, { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET } };
        w[0].dstSet = w[1].dstSet = set;
        w[0].dstBinding = 0;
        w[0].descriptorCount = 1;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[0].pImageInfo = &src;
        w[1].dstBinding = 1;
        w[1].descriptorCount = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w[1].pImageInfo = &dst;
        vkUpdateDescriptorSets(g_device, 2, w, 0, nullptr);
    }

    // Images for an inW x inH -> outW x outH upscale (recreated when a size changes).
    bool PrepareUpscale(uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH)
    {
        if (!InitUpscale()) return false;
        if (g_up.inW == inW && g_up.inH == inH && g_up.outW == outW && g_up.outH == outH && g_up.out.image) return true;
        vkDeviceWaitIdle(g_device);
        const bool ok = CreateUpscaleImage(g_up.in, inW, inH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
            CreateUpscaleImage(g_up.mid, outW, outH, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) &&
            CreateUpscaleImage(g_up.out, outW, outH, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (!ok)
        {
            DestroyUpscaleImage(g_up.in); DestroyUpscaleImage(g_up.mid); DestroyUpscaleImage(g_up.out);
            g_up.inW = g_up.inH = g_up.outW = g_up.outH = 0;
            return false;
        }
        WriteUpscaleSet(g_up.setEasu, g_up.in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, g_up.mid);
        WriteUpscaleSet(g_up.setRcas, g_up.mid, VK_IMAGE_LAYOUT_GENERAL, g_up.out);
        WriteUpscaleSet(g_up.setSgsr, g_up.in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, g_up.out);
        g_up.inW = inW; g_up.inH = inH; g_up.outW = outW; g_up.outH = outH;
        g_up.queryPending = false;
        std::printf("[video] upscaler images: %ux%u -> %ux%u\n", inW, inH, outW, outH);
        return true;
    }

    uint32_t AsU32(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

    // Upscales `source` (inW x inH, TRANSFER_SRC) into g_up.out (outW x outH, left in TRANSFER_SRC).
    void RecordUpscale(VkImage source, uint32_t inW, uint32_t inH, uint32_t outW, uint32_t outH, Video::Upscaler which)
    {
        // Last frame's timing (the fence of that frame has been waited for).
        if (g_up.queryPending)
        {
            uint64_t ts[2] = {};
            if (vkGetQueryPoolResults(g_device, g_up.queries, 0, 2, sizeof(ts), ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            {
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(g_physical, &props);
                g_up.gpuMs += double(ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
                if (++g_up.frames == 120)
                {
                    std::printf("[video] upscaler %s %ux%u -> %ux%u: %.2f ms GPU per frame\n", which == Video::Upscaler::Fsr1 ? "FSR 1" : "SGSR 1",
                        inW, inH, outW, outH, g_up.gpuMs / 120.0);
                    g_up.gpuMs = 0;
                    g_up.frames = 0;
                }
            }
        }
        vkCmdResetQueryPool(g_cmd, g_up.queries, 0, 2);
        vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_up.queries, 0);

        // Game image -> `in` (the blit also converts BGRA game images to RGBA).
        Barrier(g_up.in.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit copy{};
        copy.srcSubresource = copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.srcOffsets[1] = copy.dstOffsets[1] = { int32_t(inW), int32_t(inH), 1 };
        vkCmdBlitImage(g_cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_up.in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy, VK_FILTER_NEAREST);
        Barrier(g_up.in.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        Barrier(g_up.out.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        uint32_t pc[20] = {};
        if (which == Video::Upscaler::Fsr1)
        {
            // FsrEasuCon (ffx_fsr1.h): input viewport = input size.
            const float iw = float(inW), ih = float(inH), ow = float(outW), oh = float(outH);
            pc[0] = AsU32(iw / ow);                 pc[1] = AsU32(ih / oh);
            pc[2] = AsU32(0.5f * iw / ow - 0.5f);   pc[3] = AsU32(0.5f * ih / oh - 0.5f);
            pc[4] = AsU32(1.0f / iw);               pc[5] = AsU32(1.0f / ih);
            pc[6] = AsU32(1.0f / iw);               pc[7] = AsU32(-1.0f / ih);
            pc[8] = AsU32(-1.0f / iw);              pc[9] = AsU32(2.0f / ih);
            pc[10] = AsU32(1.0f / iw);              pc[11] = AsU32(2.0f / ih);
            pc[12] = AsU32(0.0f);                   pc[13] = AsU32(4.0f / ih);
            pc[14] = pc[15] = 0;
            pc[16] = outW; pc[17] = outH;
            Barrier(g_up.mid.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.easu);
            vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.layout, 0, 1, &g_up.setEasu, 0, nullptr);
            vkCmdPushConstants(g_cmd, g_up.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 80, pc);
            vkCmdDispatch(g_cmd, (outW + 15) / 16, (outH + 15) / 16, 1);
            Barrier(g_up.mid.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
            // FsrRcasCon: sharpness 0.2 stops (AMD's usual default), converted from stops to linear.
            pc[0] = AsU32(std::exp2(-0.2f));
            pc[1] = pc[2] = pc[3] = 0;
            vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.rcas);
            vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.layout, 0, 1, &g_up.setRcas, 0, nullptr);
            vkCmdPushConstants(g_cmd, g_up.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 80, pc);
            vkCmdDispatch(g_cmd, (outW + 15) / 16, (outH + 15) / 16, 1);
        }
        else
        {
            const float info[4] = { 1.0f / float(inW), 1.0f / float(inH), float(inW), float(inH) };
            std::memcpy(pc, info, sizeof(info));
            pc[4] = outW; pc[5] = outH;
            vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.sgsr);
            vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_up.layout, 0, 1, &g_up.setSgsr, 0, nullptr);
            vkCmdPushConstants(g_cmd, g_up.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 80, pc);
            vkCmdDispatch(g_cmd, (outW + 7) / 8, (outH + 7) / 8, 1);
        }
        Barrier(g_up.out.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_up.queries, 1);
        g_up.queryPending = true;
    }

    // its aspect ratio: scaled to fit and centred, with black bars on the sides or top/bottom (e.g. a
    // 16:9 game on the ~1.44:1 iPad screen, or a resized window). PES_STRETCH=1 fills the whole window
    // (the old behaviour). Leaves `target` in PRESENT_SRC layout.
    void RecordBlitToSwapImage(VkImage source, uint32_t width, uint32_t height, VkImage target)
    {
        static const bool stretch = [] { const char* e = std::getenv("PES_STRETCH"); return e && *e && *e != '0'; }();
        const int32_t dstW = int32_t(g_swapExtent.width), dstH = int32_t(g_swapExtent.height);
        int32_t x0 = 0, y0 = 0, x1 = dstW, y1 = dstH;
        if (!stretch)
        {
            // Fit: compare aspect ratios in integers (dstW/dstH vs width/height).
            if (int64_t(dstW) * height > int64_t(dstH) * width)
            {
                const int32_t w = int32_t((int64_t(dstH) * width + height / 2) / height); // pillarbox
                x0 = (dstW - w) / 2;
                x1 = x0 + w;
            }
            else
            {
                const int32_t h = int32_t((int64_t(dstW) * height + width / 2) / width); // letterbox
                y0 = (dstH - h) / 2;
                y1 = y0 + h;
            }
        }
        const bool bars = x0 > 0 || y0 > 0 || x1 < dstW || y1 < dstH;

        // Upscaler: only when the picture is enlarged. Its output is the on-screen size, or lower when
        // an output height is set (then scaled to the screen below with linear filtering).
        const auto which = Video::Upscaler(g_upscaler.load());
        const uint32_t fitW = uint32_t(std::max(x1 - x0, 0)), fitH = uint32_t(std::max(y1 - y0, 0));
        uint32_t upW = fitW, upH = fitH;
        if (const uint32_t limit = g_upscaleHeight.load(); limit && limit < fitH)
        {
            upH = limit;
            upW = uint32_t((uint64_t(fitW) * limit + fitH / 2) / fitH);
        }
        bool upscaled = false;
        if (which != Video::Upscaler::Off && upW > width && upH > height && PrepareUpscale(width, height, upW, upH))
        {
            RecordUpscale(source, width, height, upW, upH, which);
            source = g_up.out.image;
            width = upW;
            height = upH;
            upscaled = upW == fitW && upH == fitH; // 1:1 copy; otherwise still a linear scale
        }

        Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        if (bars)
        {
            const VkClearColorValue black{ { 0.0f, 0.0f, 0.0f, 1.0f } };
            const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(g_cmd, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
            Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }
        if (x1 > x0 && y1 > y0)
        {
            VkImageBlit blit{};
            blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.srcOffsets[1] = { int32_t(width), int32_t(height), 1 };
            blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            blit.dstOffsets[0] = { x0, y0, 0 };
            blit.dstOffsets[1] = { x1, y1, 1 };
            vkCmdBlitImage(g_cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &blit, upscaled ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        }
        Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
}

namespace Video
{
    bool Init(uint32_t width, uint32_t height, const char* title)
    {
        bool loaded = volkInitialize() == VK_SUCCESS;
#ifdef __APPLE__
        // iPadOS: MoltenVK ships inside the app (Frameworks/MoltenVK.framework); volk's own search
        // uses paths relative to the working directory.
        if (!loaded)
            if (void* lib = dlopen("@executable_path/Frameworks/MoltenVK.framework/MoltenVK", RTLD_NOW | RTLD_LOCAL))
                if (auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr")))
                {
                    volkInitializeCustom(gipa);
                    loaded = true;
                }
#endif
        if (!loaded)
        {
            std::fprintf(stderr, "[video] Vulkan loader not found - running without a window\n");
            return false;
        }

        if (!Window::Create(width, height, title))
            return false;

        std::vector<const char*> instanceExtensions = Window::InstanceExtensions();
        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "PES13";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
#ifdef __APPLE__
        // MoltenVK is a "portability" implementation: the loader lists it only when asked to.
        // Without a loader (iPadOS: MoltenVK loaded directly) the extension does not exist and
        // requesting it fails vkCreateInstance with VK_ERROR_EXTENSION_NOT_PRESENT - enable it only
        // when it is enumerated.
        {
            uint32_t count = 0;
            vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> props(count);
            vkEnumerateInstanceExtensionProperties(nullptr, &count, props.data());
            for (const VkExtensionProperties& e : props)
                if (!std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
                {
                    instanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
                    ici.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
                    break;
                }
        }
        instanceExtensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
#endif
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = uint32_t(instanceExtensions.size());
        ici.ppEnabledExtensionNames = instanceExtensions.data();
        if (!Check(vkCreateInstance(&ici, nullptr, &g_instance), "vkCreateInstance")) return false;
        volkLoadInstance(g_instance);

        g_surface = Window::CreateSurface(g_instance);
        if (!g_surface) return false;

        // Prefer a discrete GPU whose queue can present to the window.
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(g_instance, &count, nullptr);
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(g_instance, &count, devices.data());
        int bestScore = -1;
        for (VkPhysicalDevice pd : devices)
        {
            uint32_t qcount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
            std::vector<VkQueueFamilyProperties> qprops(qcount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, qprops.data());
            for (uint32_t q = 0; q < qcount; ++q)
            {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, g_surface, &present);
                if (!present || !(qprops[q].queueFlags & VK_QUEUE_GRAPHICS_BIT))
                    continue;
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(pd, &props);
                const int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > bestScore)
                {
                    bestScore = score;
                    g_physical = pd;
                    g_queueFamily = q;
                }
                break;
            }
        }
        if (!g_physical)
        {
            std::fprintf(stderr, "[video] no Vulkan device can present to the window\n");
            return false;
        }
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(g_physical, &props);
        std::printf("[video] Vulkan device: %s\n", props.deviceName);

        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        qci.queueFamilyIndex = g_queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
#ifdef __APPLE__
        {
            // Required when the device exposes it (MoltenVK).
            uint32_t n = 0;
            vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, nullptr);
            std::vector<VkExtensionProperties> exts(n);
            vkEnumerateDeviceExtensionProperties(g_physical, nullptr, &n, exts.data());
            for (const auto& e : exts)
                if (!std::strcmp(e.extensionName, "VK_KHR_portability_subset"))
                    deviceExtensions.push_back("VK_KHR_portability_subset");
        }
#endif
        // Features the GPU renderer uses (only those the device supports).
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(g_physical, &supported);
        VkPhysicalDeviceFeatures enabled{};
        enabled.fullDrawIndexUint32 = supported.fullDrawIndexUint32;
        enabled.independentBlend = supported.independentBlend;
        enabled.samplerAnisotropy = supported.samplerAnisotropy;
        enabled.imageCubeArray = supported.imageCubeArray;
        enabled.textureCompressionBC = supported.textureCompressionBC;
        enabled.depthClamp = supported.depthClamp; // draws with clipping disabled: z clamped, not clipped (Xenos)
        VkDeviceCreateInfo dci{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = uint32_t(deviceExtensions.size());
        dci.ppEnabledExtensionNames = deviceExtensions.data();
        dci.pEnabledFeatures = &enabled;
        if (!Check(vkCreateDevice(g_physical, &dci, nullptr, &g_device), "vkCreateDevice")) return false;
        volkLoadDevice(g_device);
        vkGetDeviceQueue(g_device, g_queueFamily, 0, &g_queue);

        VkCommandPoolCreateInfo pci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = g_queueFamily;
        vkCreateCommandPool(g_device, &pci, nullptr, &g_cmdPool);
        VkCommandBufferAllocateInfo cai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        cai.commandPool = g_cmdPool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(g_device, &cai, &g_cmd);
        VkFenceCreateInfo fci{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(g_device, &fci, nullptr, &g_fence);
        VkSemaphoreCreateInfo si{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        vkCreateSemaphore(g_device, &si, nullptr, &g_acquired);

        g_context.instance = g_instance;
        g_context.physical = g_physical;
        g_context.device = g_device;
        g_context.queueFamily = g_queueFamily;
        g_context.queue = g_queue;
        g_context.properties = props;
        g_context.enabledFeatures = enabled;
        vkGetPhysicalDeviceMemoryProperties(g_physical, &g_context.memory);

        g_ready = CreateSwapchain();
        return g_ready;
    }

    const VulkanContext* GetVulkanContext() { return g_device ? &g_context : nullptr; }

    namespace
    {
        std::mutex g_perfMutex;
        PerfStats g_perf;

        std::shared_mutex g_submitLock;      // shared: a submit in flight; exclusive: going to background
        std::mutex g_backgroundMutex;
        std::condition_variable g_backgroundCv;
        std::atomic<bool> g_background{ false };
    }

    void SetAppInBackground(bool background)
    {
        {
            std::lock_guard lock(g_backgroundMutex);
            g_background = background;
        }
        if (background)
        {
            // Wait for submits already past the check; new ones now wait in GpuSubmitScope.
            g_submitLock.lock();
            g_submitLock.unlock();
            std::printf("[video] app in background - GPU work paused\n");
        }
        else
        {
            g_backgroundCv.notify_all();
            std::printf("[video] app in foreground - GPU work resumed\n");
        }
    }

    GpuSubmitScope::GpuSubmitScope()
    {
        for (;;)
        {
            g_submitLock.lock_shared();
            if (!g_background.load())
                return;
            g_submitLock.unlock_shared();
            std::unique_lock lock(g_backgroundMutex);
            g_backgroundCv.wait(lock, [] { return !g_background.load(); });
        }
    }

    GpuSubmitScope::~GpuSubmitScope() { g_submitLock.unlock_shared(); }

    void SetPerfStats(const PerfStats& stats)
    {
        std::lock_guard lock(g_perfMutex);
        const uint32_t w = g_perf.frameWidth, h = g_perf.frameHeight;
        g_perf = stats;
        g_perf.frameWidth = w;
        g_perf.frameHeight = h;
    }

    PerfStats GetPerfStats()
    {
        std::lock_guard lock(g_perfMutex);
        return g_perf;
    }

    void PresentImage(VkImage image, uint32_t width, uint32_t height)
    {
        std::lock_guard lock(g_presentMutex);
        GpuSubmitScope gpuAllowed; // iPadOS: no GPU work while the app is in the background
        if (!g_device || !image || width == 0 || height == 0)
            return;
        {
            std::lock_guard perfLock(g_perfMutex);
            g_perf.frameWidth = width;
            g_perf.frameHeight = height;
        }
        if (!g_swapchain)
        {
            vkDeviceWaitIdle(g_device);
            if (!CreateSwapchain())
                return;
        }
        vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX);
        uint32_t imageIndex = 0;
        VkResult r = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, g_acquired, VK_NULL_HANDLE, &imageIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
        {
            vkDeviceWaitIdle(g_device);
            DestroySwapchain();
            return;
        }
        vkResetFences(g_device, 1, &g_fence);

        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(g_cmd, 0);
        vkBeginCommandBuffer(g_cmd, &bi);
        RecordBlitToSwapImage(image, width, height, g_swapImages[imageIndex]);
        vkEndCommandBuffer(g_cmd);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &g_acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &g_cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &g_rendered[imageIndex];
        vkQueueSubmit(g_queue, 1, &submit, g_fence);

        VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &g_rendered[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &g_swapchain;
        present.pImageIndices = &imageIndex;
        r = vkQueuePresentKHR(g_queue, &present);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        {
            vkDeviceWaitIdle(g_device);
            DestroySwapchain();
        }
    }

    void SetUpscaler(Upscaler upscaler)
    {
        if (g_upscaler.exchange(int(upscaler)) != int(upscaler))
            std::printf("[video] upscaling: %s\n", upscaler == Upscaler::Fsr1 ? "FSR 1" : upscaler == Upscaler::Sgsr1 ? "SGSR 1" : "off");
    }

    Upscaler GetUpscaler() { return Upscaler(g_upscaler.load()); }

    void SetUpscaleHeight(uint32_t height)
    {
        if (g_upscaleHeight.exchange(height) != height)
            std::printf("[video] upscaling output height: %s\n", height ? std::to_string(height).c_str() : "screen");
    }

    bool WindowHasFocus() { return Window::HasFocus(); }

    void SetTitle(const std::string& title) { Window::SetTitle(title); }

    void Present(const uint32_t* rgba, uint32_t width, uint32_t height)
    {
        std::lock_guard lock(g_presentMutex);
        GpuSubmitScope gpuAllowed; // iPadOS: no GPU work while the app is in the background
        if (!g_device || width == 0 || height == 0)
            return;

        if (!g_swapchain)
        {
            vkDeviceWaitIdle(g_device);
            if (!CreateSwapchain())
                return; // still minimized
        }
        if (width != g_uploadWidth || height != g_uploadHeight)
        {
            vkDeviceWaitIdle(g_device);
            if (!CreateUpload(width, height))
                return;
        }

        vkWaitForFences(g_device, 1, &g_fence, VK_TRUE, UINT64_MAX);

        uint32_t imageIndex = 0;
        VkResult r = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, g_acquired, VK_NULL_HANDLE, &imageIndex);
        if (r == VK_ERROR_OUT_OF_DATE_KHR)
        {
            vkDeviceWaitIdle(g_device);
            DestroySwapchain();
            return;
        }
        vkResetFences(g_device, 1, &g_fence);

        std::memcpy(g_stagingPtr, rgba, size_t(width) * height * 4);

        VkCommandBufferBeginInfo bi{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(g_cmd, 0);
        vkBeginCommandBuffer(g_cmd, &bi);

        Barrier(g_frame, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { width, height, 1 };
        vkCmdCopyBufferToImage(g_cmd, g_staging, g_frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(g_frame, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        RecordBlitToSwapImage(g_frame, width, height, g_swapImages[imageIndex]);
        vkEndCommandBuffer(g_cmd);

        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &g_acquired;
        submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &g_cmd;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &g_rendered[imageIndex];
        vkQueueSubmit(g_queue, 1, &submit, g_fence);

        VkPresentInfoKHR present{ VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &g_rendered[imageIndex];
        present.swapchainCount = 1;
        present.pSwapchains = &g_swapchain;
        present.pImageIndices = &imageIndex;
        r = vkQueuePresentKHR(g_queue, &present);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        {
            vkDeviceWaitIdle(g_device);
            DestroySwapchain();
        }
    }
}
