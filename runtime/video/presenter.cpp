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

    // Copies the game image (width x height, TRANSFER_SRC layout) onto the swap chain image, keeping
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
                1, &blit, VK_FILTER_LINEAR);
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
            // With asynchronous MoltenVK submits the Metal command buffers may still be waiting
            // to be committed: drain the queue now, while no submit can start (we hold the lock).
            if (g_queue) vkQueueWaitIdle(g_queue);
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
