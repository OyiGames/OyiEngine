#pragma once
#include <vulkan/vulkan.h>

namespace Oyi::Graphic
{
    class VulkanDebug
    {
    public:
        static void fillCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& info);
        static bool create(VkInstance instance, VkDebugUtilsMessengerEXT& messenger);
        static void destroy(VkInstance instance, VkDebugUtilsMessengerEXT messenger);
    };
}
