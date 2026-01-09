#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <Graphic/RHI/RHIContext.h>

struct SDL_Window;

namespace Oyi::Graphic
{
    class VulkanInstance;
    class VulkanSurface;
    class VulkanDevice;
    class VulkanQueue;
    class VulkanCommandPool;

    class VulkanContext final : public RHIContext
    {
    public:
        VulkanContext();
        ~VulkanContext() override;

        void setWindow(::SDL_Window* window);

        BackendType backend() const override;

        void initialize() override;
        void shutdown() override;

        VkInstance instance() const;
        VkPhysicalDevice physicalDevice() const;
        VkDevice device() const;

        VulkanQueue& graphicsQueue();
        VulkanCommandPool& graphicsCommandPool();

    private:
        std::unique_ptr<VulkanInstance> instanceObj;
        std::unique_ptr<VulkanSurface> surface;
        std::unique_ptr<VulkanDevice> deviceObj;
        std::unique_ptr<VulkanQueue> graphicsQueueObj;
        std::unique_ptr<VulkanCommandPool> graphicsCommandPoolObj;

        VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    };
}
