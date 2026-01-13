#pragma once
#include <vulkan/vulkan.h>
#include <Graphic/API.h>

namespace Oyi::Graphic
{
    class OYI_GRAPHIC_API VulkanCommandPool
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
