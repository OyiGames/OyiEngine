#pragma once
#include <vector>
#include <vulkan/vulkan.h>

struct SDL_Window;

namespace Oyi::Graphic
{
    class VulkanSurface
    {
    public:
        explicit VulkanSurface(SDL_Window* window);

        std::vector<const char*> requiredInstanceExtensions() const;

        VkSurfaceKHR create(VkInstance instance);
        void destroy(VkInstance instance);

        VkSurfaceKHR handle() const;

    private:
        SDL_Window* window;
        VkSurfaceKHR surface;
    };
}
