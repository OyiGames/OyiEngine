#include <cstdio>
#include <cstring>
#include <vulkan/vulkan.h>
#include "VulkanDebug.h"

namespace Oyi::Graphic
{
    static VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT /*type*/,
        const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
        void* /*userData*/)
    {
        const char* sev = "INFO";
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) sev = "ERROR";
        else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) sev = "WARN";
        else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) sev = "VERBOSE";

        std::fprintf(stderr, "[Vulkan][%s] %s\n", sev, callbackData && callbackData->pMessage ? callbackData->pMessage : "(null)");
        return VK_FALSE;
    }

    static VkResult CreateDebugUtilsMessengerEXT(
        VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
        const VkAllocationCallbacks* allocator,
        VkDebugUtilsMessengerEXT* messenger)
    {
        auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        if (!fn) return VK_ERROR_EXTENSION_NOT_PRESENT;
        return fn(instance, createInfo, allocator, messenger);
    }

    static void DestroyDebugUtilsMessengerEXT(
        VkInstance instance,
        VkDebugUtilsMessengerEXT messenger,
        const VkAllocationCallbacks* allocator)
    {
        auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (fn) fn(instance, messenger, allocator);
    }

    void VulkanDebug::fillCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& info)
    {
        std::memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

        info.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

        info.pfnUserCallback = DebugCallback;
        info.pUserData = nullptr;
    }

    bool VulkanDebug::create(VkInstance instance, VkDebugUtilsMessengerEXT& messenger)
    {
        VkDebugUtilsMessengerCreateInfoEXT info{};
        fillCreateInfo(info);
        VkResult r = CreateDebugUtilsMessengerEXT(instance, &info, nullptr, &messenger);
        return r == VK_SUCCESS;
    }

    void VulkanDebug::destroy(VkInstance instance, VkDebugUtilsMessengerEXT messenger)
    {
        if (instance != VK_NULL_HANDLE && messenger != VK_NULL_HANDLE)
            DestroyDebugUtilsMessengerEXT(instance, messenger, nullptr);
    }
}
