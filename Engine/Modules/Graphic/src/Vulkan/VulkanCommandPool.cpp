#include <fmt/base.h>
#include <cstdio>
#include <vulkan/vulkan.h>
#include "VulkanCommandPool.h"

namespace Oyi::Graphic
{
    void VulkanCommandPool::create(VkDevice device, uint32_t familyIndex)
    {
        if (pool != VK_NULL_HANDLE)
            return;

        VkCommandPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        ci.queueFamilyIndex = familyIndex;
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

        VkResult r = vkCreateCommandPool(device, &ci, nullptr, &pool);
        if (r != VK_SUCCESS)
        {
            fmt::print(stderr, "[OyiVulkan] vkCreateCommandPool failed: {}\n", int(r));
            pool = VK_NULL_HANDLE;
        }
    }

    void VulkanCommandPool::destroy(VkDevice device)
    {
        if (pool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(device, pool, nullptr);
            pool = VK_NULL_HANDLE;
        }
    }

    VkCommandPool VulkanCommandPool::handle() const
    {
        return pool;
    }
}
