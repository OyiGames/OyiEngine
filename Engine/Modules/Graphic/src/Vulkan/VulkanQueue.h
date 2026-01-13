#pragma once
#include <vulkan/vulkan.h>
#include <Graphic/API.h>

namespace Oyi::Graphic
{
    class OYI_GRAPHIC_API VulkanQueue
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
