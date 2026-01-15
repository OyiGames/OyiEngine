#include <vector>
#include <cstring>
#include <cstdio>
#include <fmt/base.h>
#include <vulkan/vulkan.h>
#include "VulkanDevice.h"
#include "VulkanSurface.h"


namespace Oyi::Graphic
{
    static bool HasDeviceExtension(VkPhysicalDevice pd, const char* name)
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> props(count);
        if (count) vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, props.data());

        for (auto& p : props)
            if (std::strcmp(p.extensionName, name) == 0) return true;
        return false;
    }

    static int ScoreDevice(VkPhysicalDevice pd)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);

        int score = 0;
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score += 1000;
        else if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score += 500;

        // small bias for higher limits
        score += int(props.limits.maxImageDimension2D / 1024);
        return score;
    }

    static bool FindQueueFamilies(VkPhysicalDevice pd, VkSurfaceKHR surface,
                                 uint32_t& graphicsFamily, uint32_t& presentFamily)
    {
        graphicsFamily = UINT32_MAX;
        presentFamily = UINT32_MAX;

        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        if (count) vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, families.data());

        for (uint32_t i = 0; i < count; ++i)
        {
            if (families[i].queueCount == 0) continue;

            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphicsFamily == UINT32_MAX)
                graphicsFamily = i;

            if (surface != VK_NULL_HANDLE)
            {
                VkBool32 support = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &support);
                if (support && presentFamily == UINT32_MAX)
                    presentFamily = i;
            }
        }

        if (graphicsFamily == UINT32_MAX)
            return false;

        // if no surface (headless), presentFamily not required
        if (surface == VK_NULL_HANDLE)
        {
            presentFamily = graphicsFamily;
            return true;
        }

        return presentFamily != UINT32_MAX;
    }

    VulkanDevice::VulkanDevice()
        : physicalDevice(VK_NULL_HANDLE)
        , device(VK_NULL_HANDLE)
        , graphicsQueueFamilyIndex(0)
    {
    }

    VulkanDevice::~VulkanDevice()
    {
        destroy();
    }

    void VulkanDevice::create(VkInstance instance, VulkanSurface& surfaceObj)
    {
        if (device != VK_NULL_HANDLE)
            return;

        VkSurfaceKHR surf = surfaceObj.handle();

        uint32_t pdCount = 0;
        vkEnumeratePhysicalDevices(instance, &pdCount, nullptr);
        std::vector<VkPhysicalDevice> pds(pdCount);
        if (pdCount) vkEnumeratePhysicalDevices(instance, &pdCount, pds.data());

        VkPhysicalDevice best = VK_NULL_HANDLE;
        int bestScore = -1;
        uint32_t bestGraphics = UINT32_MAX;
        uint32_t bestPresent = UINT32_MAX;

        for (auto pd : pds)
        {
            // require swapchain if we have a surface
            if (surf != VK_NULL_HANDLE && !HasDeviceExtension(pd, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
                continue;

            uint32_t g = UINT32_MAX, p = UINT32_MAX;
            if (!FindQueueFamilies(pd, surf, g, p))
                continue;

            int score = ScoreDevice(pd);
            if (score > bestScore)
            {
                bestScore = score;
                best = pd;
                bestGraphics = g;
                bestPresent = p;
            }
        }

        if (best == VK_NULL_HANDLE)
        {
            fmt::print(stderr, "[OyiVulkan] No suitable physical device found.\n");
            return;
        }

        physicalDevice = best;
        graphicsQueueFamilyIndex = bestGraphics;

        // Create VkDevice
        float queuePriority = 1.0f;

        // If present != graphics, create two queue infos
        std::vector<VkDeviceQueueCreateInfo> queueInfos;
        queueInfos.reserve(2);

        VkDeviceQueueCreateInfo q0{};
        q0.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        q0.queueFamilyIndex = bestGraphics;
        q0.queueCount = 1;
        q0.pQueuePriorities = &queuePriority;
        queueInfos.push_back(q0);

        if (bestPresent != bestGraphics)
        {
            VkDeviceQueueCreateInfo q1 = q0;
            q1.queueFamilyIndex = bestPresent;
            queueInfos.push_back(q1);
        }

        std::vector<const char*> deviceExts;
        if (surf != VK_NULL_HANDLE)
            deviceExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        VkPhysicalDeviceFeatures features{};
        // Keep minimal for now. Enable what you truly need later.

        VkDeviceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        ci.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
        ci.pQueueCreateInfos = queueInfos.data();
        ci.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
        ci.ppEnabledExtensionNames = deviceExts.empty() ? nullptr : deviceExts.data();
        ci.pEnabledFeatures = &features;

        VkResult r = vkCreateDevice(physicalDevice, &ci, nullptr, &device);
        if (r != VK_SUCCESS)
        {
            fmt::print(stderr, "[OyiVulkan] vkCreateDevice failed: {}\n", int(r));
            device = VK_NULL_HANDLE;
            physicalDevice = VK_NULL_HANDLE;
            return;
        }
    }

    void VulkanDevice::destroy()
    {
        if (device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(device);
            vkDestroyDevice(device, nullptr);
            device = VK_NULL_HANDLE;
        }
        physicalDevice = VK_NULL_HANDLE;
        graphicsQueueFamilyIndex = 0;
    }

    VkPhysicalDevice VulkanDevice::physical() const
    {
        return physicalDevice;
    }

    VkDevice VulkanDevice::logical() const
    {
        return device;
    }

    uint32_t VulkanDevice::graphicsQueueFamily() const
    {
        return graphicsQueueFamilyIndex;
    }
}
