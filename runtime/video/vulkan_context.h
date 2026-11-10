#pragma once
// The presenter's Vulkan objects, shared with the GPU renderer (one device, one queue).
// Queue use is single-threaded: both the renderer and Present run on the GPU command
// processor thread.
#include <volk.h>

namespace Video
{
    struct VulkanContext
    {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkQueue queue = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceFeatures enabledFeatures{};
        VkPhysicalDeviceMemoryProperties memory{};
    };

    // Null until Video::Init succeeded.
    const VulkanContext* GetVulkanContext();

    // Shows `image` (2D, color, currently in TRANSFER_SRC_OPTIMAL, work on it finished)
    // scaled to the window. Blocks for vsync like Present.
    void PresentImage(VkImage image, uint32_t width, uint32_t height);
}
