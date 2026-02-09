add_subdirectory(${CMAKE_SOURCE_DIR}/Engine/Packages/NRI)

set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Single-config generator build type: Debug for better symbols and diagnostics.")

# Output/configuration paths.
set(NRI_SHADERS_PATH "${CMAKE_SOURCE_DIR}/_Shaders" CACHE STRING "Default shader output path; keep local to the repo for submodule builds.")
set(NRI_AGILITY_SDK_DIR "AgilitySDK" CACHE STRING "Relative output dir for Agility SDK binaries next to the shared NRI DLL.")
set(NRI_NVAPI_CUSTOM_PATH "" CACHE STRING "Leave empty to use bundled NVAPI; set only if a custom NVAPI is required.")

# Build as a shared library so the submodule exposes a dynamic NRI binary to dependents.
set(NRI_STATIC_LIBRARY OFF CACHE BOOL "OFF builds a shared NRI library for submodule reuse.")

# Debug/diagnostic features.
set(NRI_ENABLE_DEBUG_NAMES_AND_ANNOTATIONS ON CACHE BOOL "Keep debug names and annotations for easier GPU/CPU debugging.")
set(NRI_ENABLE_NVTX_SUPPORT ON CACHE BOOL "Enable NVTX markers so Nsight Systems can show NRI ranges in Debug.")
set(NRI_ENABLE_VALIDATION_SUPPORT ON CACHE BOOL "Include the NRI validation layer to catch API misuse in Debug builds.")
set(NRI_ENABLE_IMGUI_EXTENSION OFF CACHE BOOL "Keep ImGui extension OFF by default; enable only if the project uses it.")
set(NRI_ENABLE_NIS_SDK OFF CACHE BOOL "Disable NIS SDK by default to avoid extra SDK dependency in Debug.")

# Backends and extensions: keep broad coverage for debugging across APIs.
set(NRI_ENABLE_NONE_SUPPORT ON CACHE BOOL "Keep NONE backend available for headless/testing scenarios.")
set(NRI_ENABLE_VK_SUPPORT ON CACHE BOOL "Enable Vulkan backend so Debug builds can validate VK paths.")
set(NRI_ENABLE_D3D11_SUPPORT ON CACHE BOOL "Enable D3D11 backend for legacy/compat testing on Windows.")
set(NRI_ENABLE_D3D12_SUPPORT ON CACHE BOOL "Enable D3D12 backend for modern Windows debugging.")
set(NRI_ENABLE_AMDAGS ON CACHE BOOL "Enable AMD AGS on Windows for extra D3D debug tooling (ignored elsewhere).")
set(NRI_ENABLE_NVAPI ON CACHE BOOL "Enable NVAPI on Windows for NVIDIA-specific D3D features (ignored elsewhere).")
set(NRI_ENABLE_AGILITY_SDK_SUPPORT ON CACHE BOOL "Enable Agility SDK to access newer D3D12 features in Debug on Windows.")
set(NRI_ENABLE_XLIB_SUPPORT ON CACHE BOOL "Enable X11 WSI for Vulkan builds on Linux when headers are present.")
set(NRI_ENABLE_WAYLAND_SUPPORT ON CACHE BOOL "Enable Wayland WSI for Vulkan builds on Linux when headers are present.")
set(NRI_ENABLE_NGX_SDK OFF CACHE BOOL "Disable NGX/DLSS by default; enable only when the SDK is available.")
set(NRI_ENABLE_FFX_SDK OFF CACHE BOOL "Disable FidelityFX SDK by default; enable only when integrating FFX.")
set(NRI_ENABLE_XESS_SDK OFF CACHE BOOL "Disable XeSS SDK by default; enable only when integrating XeSS.")

# Thread-safety vs. performance: keep safer behavior in Debug.