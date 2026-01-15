#include <cstdio>
#include <vector>
#include <fmt/base.h>
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

        if (!surface)
        {
            fmt::print(stderr, "[OyiVulkan] VulkanContext::initialize requires surface to be created with a valid SDL_Window.\n");
            return;
        }

        std::vector<const char*> exts = surface->requiredInstanceExtensions();

        // Create instance
        instanceObj = std::make_unique<VulkanInstance>();
        instanceObj->create("OyiEngine", exts, DefaultEnableValidation);
        if (instanceObj->handle() == VK_NULL_HANDLE)
        {
            fmt::print(stderr, "[OyiVulkan] Failed to create VkInstance.\n");
            instanceObj.reset();
            return;
        }

        if (DefaultEnableValidation)
        {
            VulkanDebug::create(instanceObj->handle(), debugMessenger);
        }

        // Create surface handle
        surface->create(instanceObj->handle());
        if (surface->handle() == VK_NULL_HANDLE)
        {
            fmt::print(stderr, "[OyiVulkan] Failed to create VkSurfaceKHR.\n");
            shutdown();
            return;
        }

        // Create device
        deviceObj = std::make_unique<VulkanDevice>();
        deviceObj->create(instanceObj->handle(), *surface);
        if (deviceObj->logical() == VK_NULL_HANDLE)
        {
            fmt::print(stderr, "[OyiVulkan] Failed to create VkDevice.\n");
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
