// Shim for Xenia's VulkanDevice: just the properties the SPIR-V translator's Features read.
// The runtime fills one from its own Vulkan device (see runtime/gpu).
#pragma once
#include <cstdint>
#include <vulkan/vulkan_core.h>

namespace xe::ui::vulkan
{
    class VulkanDevice
    {
    public:
        struct Properties
        {
            uint32_t apiVersion = VK_API_VERSION_1_0;
            uint32_t maxStorageBufferRange = 1u << 27;
            bool fullDrawIndexUint32 = true;
            bool vertexPipelineStoresAndAtomics = false;
            bool fragmentStoresAndAtomics = false;
            bool shaderClipDistance = true;
            bool shaderCullDistance = false;
            bool imageViewFormatSwizzle = true;
            bool shaderSignedZeroInfNanPreserveFloat32 = false;
            bool shaderDenormFlushToZeroFloat32 = false;
            bool shaderRoundingModeRTEFloat32 = false;
            bool fragmentShaderSampleInterlock = false;
            bool shaderDemoteToHelperInvocation = false;
        };
        struct Extensions
        {
            bool ext_1_2_KHR_spirv_1_4 = false;
        };

        const Properties& properties() const { return properties_; }
        const Extensions& extensions() const { return extensions_; }
        Properties& mutable_properties() { return properties_; }
        Extensions& mutable_extensions() { return extensions_; }

    private:
        Properties properties_;
        Extensions extensions_;
    };
}
