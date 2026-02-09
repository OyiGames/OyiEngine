add_subdirectory(${CMAKE_SOURCE_DIR}/Engine/Packages/NRI)

set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Single-config generator build type: Release for optimized binaries.")

# Build as a shared library so the submodule exposes a dynamic NRI binary to dependents.
set(NRI_STATIC_LIBRARY OFF CACHE BOOL "OFF builds a shared NRI library for submodule reuse.")

# Output/configuration paths.
set(NRI_SHADERS_PATH "${CMAKE_SOURCE_DIR}/_Shaders" CACHE STRING "Default shader output path; keep local to the repo for submodule builds.")
set(NRI_AGILITY_SDK_DIR "AgilitySDK" CACHE STRING "Relative output dir for Agility SDK binaries next to the shared NRI DLL.")
set(NRI_NVAPI_CUSTOM_PATH "" CACHE STRING "Leave empty to use bundled NVAPI; set only if a custom NVAPI is required.")

# Debug/diagnostic features: disable to reduce overhead in Release.
set(NRI_ENABLE_DEBUG_NAMES_AND_ANNOTATIONS OFF CACHE BOOL "Disable debug names/annotations to avoid runtime overhead in Release.")
set(NRI_ENABLE_NVTX_SUPPORT OFF CACHE BOOL "Disable NVTX markers to minimize Release instrumentation cost.")
set(NRI_ENABLE_VALIDATION_SUPPORT OFF CACHE BOOL "Exclude validation layer for best Release performance.")
set(NRI_ENABLE_IMGUI_EXTENSION OFF CACHE BOOL "Keep ImGui extension OFF by default; enable only if the project uses it.")
set(NRI_ENABLE_NIS_SDK OFF CACHE BOOL "Disable NIS SDK by default to avoid extra SDK dependency in Release.")

# Backends and extensions: keep broad coverage for production across APIs.
set(NRI_ENABLE_NONE_SUPPORT ON CACHE BOOL "Keep NONE backend available for headless/testing scenarios.")
set(NRI_ENABLE_VK_SUPPORT ON CACHE BOOL "Enable Vulkan backend for production VK builds.")
set(NRI_ENABLE_D3D11_SUPPORT ON CACHE BOOL "Enable D3D11 backend for legacy/compat production on Windows.")
set(NRI_ENABLE_D3D12_SUPPORT ON CACHE BOOL "Enable D3D12 backend for modern Windows production.")
set(NRI_ENABLE_AMDAGS ON CACHE BOOL "Enable AMD AGS on Windows for vendor tooling (ignored elsewhere).")
set(NRI_ENABLE_NVAPI ON CACHE BOOL "Enable NVAPI on Windows for NVIDIA-specific D3D features (ignored elsewhere).")
set(NRI_ENABLE_AGILITY_SDK_SUPPORT ON CACHE BOOL "Enable Agility SDK to access newer D3D12 features in Release on Windows.")
set(NRI_ENABLE_XLIB_SUPPORT ON CACHE BOOL "Enable X11 WSI for Vulkan builds on Linux when headers are present.")
set(NRI_ENABLE_WAYLAND_SUPPORT ON CACHE BOOL "Enable Wayland WSI for Vulkan builds on Linux when headers are present.")
set(NRI_ENABLE_NGX_SDK OFF CACHE BOOL "Disable NGX/DLSS by default; enable only when the SDK is available.")
set(NRI_ENABLE_FFX_SDK OFF CACHE BOOL "Disable FidelityFX SDK by default; enable only when integrating FFX.")
set(NRI_ENABLE_XESS_SDK OFF CACHE BOOL "Disable XeSS SDK by default; enable only when integrating XeSS.")

# Thread-safety vs. performance: leave safe by default; projects can opt out if needed.
set(NRI_STREAMER_THREAD_SAFE ON CACHE BOOL "Keep thread-safe streamer by default to avoid data races in Release.")