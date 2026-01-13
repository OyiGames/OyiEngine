#pragma once
#include <vector>
#include <vulkan/vulkan.h>
#include <Graphic/API.h>

namespace Oyi::Graphic
{
    class OYI_GRAPHIC_API VulkanInstance
    {
    public:
        VulkanInstance();
        ~VulkanInstance();

        void create(
            const char* appName,
            const std::vector<const char*>& extensions,
            bool enableValidation
        );

        void destroy();

        VkInstance handle() const;

    private:
        VkInstance instance;
    };
}
