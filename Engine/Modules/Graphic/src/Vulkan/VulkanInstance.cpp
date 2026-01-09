#include <vector>
#include <cstring>
#include <cstdio>
#include <vulkan/vulkan.h>
#include "VulkanInstance.h"
#include "VulkanDebug.h"


namespace Oyi::Graphic
{
    static bool HasLayer(const std::vector<VkLayerProperties>& props, const char* name)
    {
        for (auto& p : props)
            if (std::strcmp(p.layerName, name) == 0) return true;
        return false;
    }

    static bool HasExtension(const std::vector<VkExtensionProperties>& props, const char* name)
    {
        for (auto& p : props)
            if (std::strcmp(p.extensionName, name) == 0) return true;
        return false;
    }

    VulkanInstance::VulkanInstance()
        : instance(VK_NULL_HANDLE)
    {
    }

    VulkanInstance::~VulkanInstance()
    {
        destroy();
    }

    void VulkanInstance::create(const char* appName,
                               const std::vector<const char*>& extensions,
                               bool enableValidation)
    {
        if (instance != VK_NULL_HANDLE)
            return;

        // Enumerate layers
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> layers(layerCount);
        if (layerCount)
            vkEnumerateInstanceLayerProperties(&layerCount, layers.data());

        // Enumerate extensions
        uint32_t extCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        if (extCount)
            vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());

        std::vector<const char*> enabledLayers;
        std::vector<const char*> enabledExts = extensions;

        // Validation layer + debug utils
        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (enableValidation)
        {
            const char* validationLayer = "VK_LAYER_KHRONOS_validation";
            if (HasLayer(layers, validationLayer))
                enabledLayers.push_back(validationLayer);

            // debug utils extension
            const char* dbgExt = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
            if (HasExtension(exts, dbgExt))
                enabledExts.push_back(dbgExt);

            VulkanDebug::fillCreateInfo(debugCreateInfo);
        }

        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = appName ? appName : "Oyi";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "Oyi";
        app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app.apiVersion = VK_API_VERSION_1_3; // you can downgrade if needed

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = static_cast<uint32_t>(enabledExts.size());
        ci.ppEnabledExtensionNames = enabledExts.empty() ? nullptr : enabledExts.data();
        ci.enabledLayerCount = static_cast<uint32_t>(enabledLayers.size());
        ci.ppEnabledLayerNames = enabledLayers.empty() ? nullptr : enabledLayers.data();

        if (enableValidation && !enabledLayers.empty())
            ci.pNext = &debugCreateInfo;

        VkResult r = vkCreateInstance(&ci, nullptr, &instance);
        if (r != VK_SUCCESS)
        {
            std::fprintf(stderr, "[Vulkan] vkCreateInstance failed: %d\n", int(r));
            instance = VK_NULL_HANDLE;
        }
    }

    void VulkanInstance::destroy()
    {
        if (instance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(instance, nullptr);
            instance = VK_NULL_HANDLE;
        }
    }

    VkInstance VulkanInstance::handle() const
    {
        return instance;
    }
}
