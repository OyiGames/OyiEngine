#include <cstdio>
#include <vector>
#include <vulkan/vulkan.h>
#include "VulkanContext.h"
#include "VulkanInstance.h"
#include "VulkanSurface.h"
#include "VulkanDevice.h"
#include "VulkanQueue.h"
#include "VulkanCommandPool.h"
#include "VulkanDebug.h"


namespace Oyi::Graphic
{
    // You can move this to a config object later.
    static constexpr bool DefaultEnableValidation = true;

    VulkanContext::VulkanContext() = default;

    VulkanContext::~VulkanContext()
    {
        shutdown();
    }

    void VulkanContext::setWindow(::SDL_Window* window)
    {
        surface = std::make_unique<VulkanSurface>(window);
    }

    BackendType VulkanContext::backend() const
    {
        return BackendType::Vulkan;
    }

    void VulkanContext::initialize()
    {
        if (instanceObj)
            return;

        // NOTE: In your engine, SDL_Window should come from your platform module.
        // Here we assume you have a way to pass it in or set it globally.
        // For now, VulkanSurface is constructed elsewhere or you can add a setter.
        //
        // Minimal practical approach:
        // - Provide a VulkanContext::setWindow(SDL_Window*) API
        //   then create VulkanSurface here.
        //
        // Since your header earlier didn't include that, this cpp assumes:
        // surface is already set by you before initialize() OR you will add a window setter.

        if (!surface)
        {
            std::fprintf(stderr, "[Vulkan] VulkanContext::initialize requires surface to be created with a valid SDL_Window.\n");
            return;
        }

        std::vector<const char*> exts = surface->requiredInstanceExtensions();

        // Create instance
        instanceObj = std::make_unique<VulkanInstance>();
        instanceObj->create("OyiEngine", exts, DefaultEnableValidation);
        if (instanceObj->handle() == VK_NULL_HANDLE)
        {
            std::fprintf(stderr, "[Vulkan] Failed to create VkInstance.\n");
            instanceObj.reset();
            return;
        }

        // Create debug messenger (optional)
        VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
        if (DefaultEnableValidation)
        {
            VulkanDebug::create(instanceObj->handle(), debugMessenger);
        }

        // Create surface handle
        surface->create(instanceObj->handle());
        if (surface->handle() == VK_NULL_HANDLE)
        {
            std::fprintf(stderr, "[Vulkan] Failed to create VkSurfaceKHR.\n");
            shutdown();
            return;
        }

        // Create device
        deviceObj = std::make_unique<VulkanDevice>();
        deviceObj->create(instanceObj->handle(), *surface);
        if (deviceObj->logical() == VK_NULL_HANDLE)
        {
            std::fprintf(stderr, "[Vulkan] Failed to create VkDevice.\n");
            shutdown();
            return;
        }

        // Queue
        graphicsQueueObj = std::make_unique<VulkanQueue>();
        graphicsQueueObj->init(deviceObj->logical(), deviceObj->graphicsQueueFamily());

        // Command pool
        graphicsCommandPoolObj = std::make_unique<VulkanCommandPool>();
        graphicsCommandPoolObj->create(deviceObj->logical(), graphicsQueueObj->family());
    }

    void VulkanContext::shutdown()
    {
        if (deviceObj && deviceObj->logical() != VK_NULL_HANDLE)
        {
            if (graphicsCommandPoolObj)
            {
                graphicsCommandPoolObj->destroy(deviceObj->logical());
                graphicsCommandPoolObj.reset();
            }

            graphicsQueueObj.reset();

            deviceObj->destroy();
            deviceObj.reset();
        }

        if (surface && instanceObj)
        {
            surface->destroy(instanceObj->handle());
            // surface object lifetime is yours; keep it if you want reuse, or reset it:
            // surface.reset();
        }

        if (instanceObj && debugMessenger != VK_NULL_HANDLE)
        {
            VulkanDebug::destroy(instanceObj->handle(), debugMessenger);
            debugMessenger = VK_NULL_HANDLE;
        }

        if (instanceObj)
        {
            instanceObj->destroy();
            instanceObj.reset();
        }
    }

    VkInstance VulkanContext::instance() const
    {
        return instanceObj ? instanceObj->handle() : VK_NULL_HANDLE;
    }

    VkPhysicalDevice VulkanContext::physicalDevice() const
    {
        return deviceObj ? deviceObj->physical() : VK_NULL_HANDLE;
    }

    VkDevice VulkanContext::device() const
    {
        return deviceObj ? deviceObj->logical() : VK_NULL_HANDLE;
    }

    VulkanQueue& VulkanContext::graphicsQueue()
    {
        return *graphicsQueueObj;
    }

    VulkanCommandPool& VulkanContext::graphicsCommandPool()
    {
        return *graphicsCommandPoolObj;
    }
}
