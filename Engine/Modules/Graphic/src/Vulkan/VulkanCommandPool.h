#pragma once
#include <vulkan/vulkan.h>

namespace Oyi::Graphic
{
    class VulkanCommandPool
    {
    public:
        VulkanCommandPool() = default;

        void create(VkDevice device, uint32_t familyIndex);
        void destroy(VkDevice device);

        VkCommandPool handle() const;

    private:
        VkCommandPool pool;
    };
}
