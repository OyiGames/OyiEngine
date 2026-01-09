#pragma once
#include <vulkan/vulkan.h>

namespace Oyi::Graphic
{
    class VulkanQueue
    {
    public:
        VulkanQueue() = default;

        void init(VkDevice device, uint32_t familyIndex);

        VkQueue handle() const;
        uint32_t family() const;

    private:
        VkQueue queue;
        uint32_t familyIndex;
    };
}
