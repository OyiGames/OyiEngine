#include <gtest/gtest.h>

#include <SDL3/SDL_vulkan.h>
#include <SDL3/SDL.h>

#include <vulkan/vulkan.h>

// Vulkan implementation headers are intentionally located under src/Vulkan.
// The test target must add Engine/Modules/Graphic/src to its include paths.
#include "Vulkan/VulkanContext.h"
#include "Vulkan/VulkanQueue.h"
#include "Vulkan/VulkanCommandPool.h"

// Vulkan Memory Allocator (VMA)
// IMPORTANT:
// Do NOT define VMA_IMPLEMENTATION in test translation units.
// The implementation must be compiled exactly once inside the Graphic module.
#include <vk_mem_alloc.h>

using namespace Oyi::Graphic;

namespace
{

// Test fixture responsible for initializing and shutting down SDL,
// and creating a Vulkan-capable SDL window for each test.
class SDLVulkanFixture : public testing::Test
{
protected:
    // Called once before all tests in this fixture.
    static void SetUpTestSuite()
    {
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
        {
            // SDL initialization failed.
            // Individual tests will skip accordingly.
            s_sdlInitOk = false;
            return;
        }
        s_sdlInitOk = true;
    }

    // Called once after all tests in this fixture.
    static void TearDownTestSuite()
    {
        if (s_sdlInitOk)
            SDL_Quit();
    }

    // Called before each test.
    void SetUp() override
    {
        if (!s_sdlInitOk)
            GTEST_SKIP() << "SDL_Init failed; skipping Vulkan tests.";

        // Create an SDL window capable of Vulkan surface creation.
        // The window is hidden to avoid UI popups during automated tests.
        window = SDL_CreateWindow(
            "Oyi Vulkan Test",
            640,
            360,
            SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN
        );

        if (!window)
            GTEST_SKIP() << "SDL_CreateWindow failed: " << SDL_GetError();

        // Sanity check: ensure SDL can provide Vulkan instance extensions.
        Uint32 extensionCount = 0;
        const char* const* extensions =
            SDL_Vulkan_GetInstanceExtensions(&extensionCount);

        if (!extensions || extensionCount == 0)
        {
            GTEST_SKIP()
                << "SDL did not provide Vulkan instance extensions. "
                << "Vulkan runtime may be unavailable on this system.";
        }
    }

    // Called after each test.
    void TearDown() override
    {
        if (window)
        {
            SDL_DestroyWindow(window);
            window = nullptr;
        }
    }

protected:
    SDL_Window* window = nullptr;

private:
    static inline bool s_sdlInitOk = false;
};

// Helper utilities for handle validation.
static bool IsValid(VkInstance h)     { return h != VK_NULL_HANDLE; }
static bool IsValid(VkDevice h)       { return h != VK_NULL_HANDLE; }
static bool IsValid(VkCommandPool h)  { return h != VK_NULL_HANDLE; }

} // anonymous namespace

// ----------------------------------------------------------------------------
// VulkanContext initialization test
// ----------------------------------------------------------------------------
TEST_F(SDLVulkanFixture, ContextInitializeCreatesCoreVulkanObjects)
{
    VulkanContext context;
    context.setWindow(window);
    context.initialize();

    // If Vulkan is not available on the system, the instance may be null.
    if (!IsValid(context.instance()))
    {
        GTEST_SKIP()
            << "Failed to create VkInstance. "
            << "Vulkan loader or driver may be missing.";
    }

    // Core Vulkan objects
    EXPECT_TRUE(IsValid(context.instance()));
    EXPECT_NE(context.physicalDevice(), VK_NULL_HANDLE);
    EXPECT_TRUE(IsValid(context.device()));

    // Graphics queue and command pool
    EXPECT_NE(context.graphicsQueue().handle(), VK_NULL_HANDLE);
    EXPECT_TRUE(IsValid(context.graphicsCommandPool().handle()));

    context.shutdown();
}

// ----------------------------------------------------------------------------
// VMA integration test
// ----------------------------------------------------------------------------
TEST_F(SDLVulkanFixture, VMA_CanAllocateAndFreeBuffer)
{
    VulkanContext context;
    context.setWindow(window);
    context.initialize();

    if (!IsValid(context.instance()) ||
        !IsValid(context.device()) ||
        context.physicalDevice() == VK_NULL_HANDLE)
    {
        GTEST_SKIP() << "VulkanContext not fully initialized; skipping VMA test.";
    }

    // Create a VMA allocator bound to the VulkanContext.
    VmaAllocator allocator = VK_NULL_HANDLE;

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.instance = context.instance();
    allocatorInfo.physicalDevice = context.physicalDevice();
    allocatorInfo.device = context.device();

    VkResult result = vmaCreateAllocator(&allocatorInfo, &allocator);
    if (result != VK_SUCCESS || allocator == VK_NULL_HANDLE)
    {
        GTEST_SKIP()
            << "vmaCreateAllocator failed. "
            << "VMA may not be linked or Vulkan functions are unavailable.";
    }

    // Create a small test buffer.
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size  = 1024;
    bufferInfo.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
        VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;
    allocationInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

    result = vmaCreateBuffer(
        allocator,
        &bufferInfo,
        &allocationInfo,
        &buffer,
        &allocation,
        nullptr
    );

    EXPECT_EQ(result, VK_SUCCESS);
    EXPECT_NE(buffer, VK_NULL_HANDLE);
    EXPECT_NE(allocation, VK_NULL_HANDLE);

    // Cleanup resources.
    if (buffer != VK_NULL_HANDLE && allocation != VK_NULL_HANDLE)
        vmaDestroyBuffer(allocator, buffer, allocation);

    vmaDestroyAllocator(allocator);

    context.shutdown();
}
