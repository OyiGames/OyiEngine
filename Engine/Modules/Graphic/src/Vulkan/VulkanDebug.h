#pragma once
#include <vulkan/vulkan.h>
#include <Graphic/API.h>

namespace Oyi::Graphic
{
    class OYI_GRAPHIC_API VulkanDebug
    {
    public:
        static void fillCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& info);
        static bool create(VkInstance instance, VkDebugUtilsMessengerEXT& messenger);
        static void destroy(VkInstance instance, VkDebugUtilsMessengerEXT messenger);
    };
}
