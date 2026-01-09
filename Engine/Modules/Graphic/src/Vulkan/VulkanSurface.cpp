#include <cstdio>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include "VulkanSurface.h"



namespace Oyi::Graphic
{
    VulkanSurface::VulkanSurface(SDL_Window* wnd)
        : window(wnd)
        , surface(VK_NULL_HANDLE)
    {
    }

    std::vector<const char*> VulkanSurface::requiredInstanceExtensions() const
    {
        std::vector<const char*> out;

        Uint32 count = 0;
        const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&count);
        if (!exts || count == 0)
            return out;

        out.reserve(count);
        for (Uint32 i = 0; i < count; ++i)
            out.push_back(exts[i]);

        return out;
    }

    VkSurfaceKHR VulkanSurface::create(VkInstance instance)
    {
        if (surface != VK_NULL_HANDLE)
            return surface;

        if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface))
        {
            std::fprintf(stderr, "[Vulkan] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
            surface = VK_NULL_HANDLE;
        }

        return surface;
    }

    void VulkanSurface::destroy(VkInstance instance)
    {
        if (surface != VK_NULL_HANDLE && instance != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
    }

    VkSurfaceKHR VulkanSurface::handle() const
    {
        return surface;
    }
}
