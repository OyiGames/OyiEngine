# ============================================================
# VulkanMemoryAllocator (VMA) - Release configuration
# ============================================================

set(OYI_VMA_DIR ${CMAKE_SOURCE_DIR}/Engine/Packages/VulkanMemoryAllocator)
set(OYI_VMA_INCLUDE_DIR ${OYI_VMA_DIR}/include)

if (NOT EXISTS ${OYI_VMA_INCLUDE_DIR}/vk_mem_alloc.h)
    message(FATAL_ERROR
        "VulkanMemoryAllocator not found at:\n"
        "  ${OYI_VMA_INCLUDE_DIR}/vk_mem_alloc.h\n"
        "Did you forget to run:\n"
        "  git submodule update --init --recursive ?"
    )
endif()

find_package(Vulkan REQUIRED)

# ---- bring VMA target (no :: for maximum compatibility) ----
if (NOT TARGET VMA)
    add_library(VMA INTERFACE)
    target_include_directories(VMA INTERFACE ${OYI_VMA_INCLUDE_DIR})
    target_link_libraries(VMA INTERFACE Vulkan::Vulkan)
endif()

# ---- optional alias (only if your CMake allows it) ----
# Some older CMake reject namespaced custom targets. Alias is also a target name,
# so guard it similarly.
if (NOT TARGET VMA::VMA)
    # If this line errors in your CMake, comment it out and link to VMA directly.
    add_library(VMA::VMA ALIAS VMA)
endif()
