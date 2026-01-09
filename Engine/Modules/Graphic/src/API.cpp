// Engine/Graphic/src/API.cpp
// Implements the stable C ABI defined in Engine/Graphic/Public/API.h.

#include <Graphic/API.h>

#include <new>          // std::nothrow
#include <cstring>      // std::memset

// Backend implementation headers (internal)
#include <Graphic/Vulkan/VulkanContext.h>
#include <Graphic/Vulkan/VulkanQueue.h>

namespace
{
    thread_local const char* g_lastErrorCStr = nullptr;

    // Store pointers to static strings only (or ensure lifetime is static).
    // For now, keep it simple and stable (no std::string across DLL boundary).
    void SetLastError(const char* msg) noexcept
    {
        g_lastErrorCStr = msg;
    }

    void ClearLastError() noexcept
    {
        g_lastErrorCStr = nullptr;
    }
}

// Opaque handle definition (ABI-stable: pointer-sized, never exposed fields)
struct OYI_GFX_Device_T
{
    uint32_t backend = 0;

    // Currently only Vulkan is implemented.
    // Stored as void* to avoid C++ type exposure at ABI boundary.
    void* impl = nullptr;
};

extern "C"
{
    uint32_t OYI_GFX_GetApiVersion(void)
    {
        return OYI_GFX_API_VERSION_1;
    }

    const char* OYI_GFX_GetLastErrorMessage(void)
    {
        return g_lastErrorCStr;
    }

    OYI_GFX_Result OYI_GFX_CreateDevice(const OYI_GFX_DeviceDesc* desc, OYI_GFX_Device* outDevice)
    {
        ClearLastError();

        if (!desc || !outDevice)
        {
            SetLastError("OYI_GFX_CreateDevice: invalid argument.");
            return OYI_GFX_E_INVALID_ARG;
        }

        *outDevice = nullptr;

        if (desc->apiVersion != OYI_GFX_API_VERSION_1)
        {
            SetLastError("OYI_GFX_CreateDevice: unsupported apiVersion.");
            return OYI_GFX_E_UNSUPPORTED;
        }

        if (desc->backend != OYI_GFX_BACKEND_VULKAN)
        {
            SetLastError("OYI_GFX_CreateDevice: backend not supported.");
            return OYI_GFX_E_UNSUPPORTED;
        }

        // Allocate ABI handle
        OYI_GFX_Device_T* dev = new (std::nothrow) OYI_GFX_Device_T();
        if (!dev)
        {
            SetLastError("OYI_GFX_CreateDevice: out of memory allocating device handle.");
            return OYI_GFX_E_INIT_FAILED;
        }
        dev->backend = desc->backend;

        // Create Vulkan backend context
        auto* ctx = new (std::nothrow) Oyi::Graphic::VulkanContext();
        if (!ctx)
        {
            delete dev;
            SetLastError("OYI_GFX_CreateDevice: out of memory allocating VulkanContext.");
            return OYI_GFX_E_INIT_FAILED;
        }

        // desc->window is expected to be SDL_Window* (SDL3). VulkanContext currently forward-declares
        // SDL_Window under Oyi::Graphic namespace, so we must cast through void* safely.
        if (desc->window)
        {
            ctx->setWindow(reinterpret_cast<Oyi::Graphic::SDL_Window*>(desc->window));
        }

        // NOTE:
        // Your current VulkanContext::initialize() uses an internal DefaultEnableValidation constant.
        // desc->enableValidation is therefore not wired yet (requires a small VulkanContext refactor).
        ctx->initialize();

        if (ctx->instance() == VK_NULL_HANDLE || ctx->physicalDevice() == VK_NULL_HANDLE || ctx->device() == VK_NULL_HANDLE)
        {
            // Initialization failed; ensure cleanup
            ctx->shutdown();
            delete ctx;
            delete dev;

            // If VulkanContext printed a more specific error, you can later bridge it here.
            SetLastError("OYI_GFX_CreateDevice: Vulkan initialization failed (instance/device invalid).");
            return OYI_GFX_E_INIT_FAILED;
        }

        dev->impl = ctx;
        *outDevice = dev;
        return OYI_GFX_OK;
    }

    void OYI_GFX_DestroyDevice(OYI_GFX_Device device)
    {
        ClearLastError();

        if (!device)
            return;

        auto* dev = reinterpret_cast<OYI_GFX_Device_T*>(device);

        if (dev->backend == OYI_GFX_BACKEND_VULKAN && dev->impl)
        {
            auto* ctx = reinterpret_cast<Oyi::Graphic::VulkanContext*>(dev->impl);
            ctx->shutdown();
            delete ctx;
            dev->impl = nullptr;
        }

        delete dev;
    }

    OYI_GFX_Result OYI_GFX_GetVulkanNative(OYI_GFX_Device device, OYI_GFX_VulkanNative* outNative)
    {
        ClearLastError();

        if (!device || !outNative)
        {
            SetLastError("OYI_GFX_GetVulkanNative: invalid argument.");
            return OYI_GFX_E_INVALID_ARG;
        }

        auto* dev = reinterpret_cast<OYI_GFX_Device_T*>(device);
        if (dev->backend != OYI_GFX_BACKEND_VULKAN || !dev->impl)
        {
            SetLastError("OYI_GFX_GetVulkanNative: device is not Vulkan.");
            return OYI_GFX_E_UNSUPPORTED;
        }

        auto* ctx = reinterpret_cast<Oyi::Graphic::VulkanContext*>(dev->impl);

        std::memset(outNative, 0, sizeof(*outNative));
        outNative->instance       = reinterpret_cast<OYI_VkInstance>(ctx->instance());
        outNative->physicalDevice = reinterpret_cast<OYI_VkPhysicalDevice>(ctx->physicalDevice());
        outNative->device         = reinterpret_cast<OYI_VkDevice>(ctx->device());

        // Queue + family index
        // (Assumes initialize() created the graphics queue. If not, this would be UB; so keep it after init.)
        {
            auto& q = ctx->graphicsQueue();
            outNative->graphicsQueue = reinterpret_cast<OYI_VkQueue>(q.handle());
            outNative->graphicsQueueFamilyIndex = q.family();
        }

        // Surface handle:
        // Your VulkanContext currently does not expose VkSurfaceKHR publicly.
        // Leave it as 0 for now; the smoke test does not require it.
        outNative->surface = 0;

        return OYI_GFX_OK;
    }
}
