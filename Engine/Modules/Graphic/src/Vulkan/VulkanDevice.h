#pragma once
#include <vulkan/vulkan.h>
#include <Graphic/API.h>

namespace Oyi::Graphic
{
    class VulkanSurface;

    class OYI_GRAPHIC_API VulkanDevice
    {
    public:
        VulkanDevice();
        ~VulkanDevice();

        void create(VkInstance instance, VulkanSurface& surface);
        void destroy();

        VkPhysicalDevice physical() const;
        VkDevice logical() const;

        uint32_t graphicsQueueFamily() const;

    private:
        VkPhysicalDevice physicalDevice;
        VkDevice device;
        uint32_t graphicsQueueFamilyIndex;
    };
}
