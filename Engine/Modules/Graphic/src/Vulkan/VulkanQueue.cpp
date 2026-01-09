#include <vulkan/vulkan.h>
#include "VulkanQueue.h"

namespace Oyi::Graphic
{
    void VulkanQueue::init(VkDevice device, uint32_t fam)
    {
        familyIndex = fam;
        vkGetDeviceQueue(device, fam, 0, &queue);
    }

    VkQueue VulkanQueue::handle() const
    {
        return queue;
    }

    uint32_t VulkanQueue::family() const
    {
        return familyIndex;
    }
}
