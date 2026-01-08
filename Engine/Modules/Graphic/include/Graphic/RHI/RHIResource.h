#pragma once
#include <cstdint>
#include <string>

namespace Oyi::Graphic 
{
using RHIResourceID = uint32_t;
static constexpr RHIResourceID InvalidRHIResource = 0;

enum class RHIResourceType : uint8_t
{
    Buffer,
    Texture
};

// Backend-agnostic description used by the render-graph front-end
enum class RHITextureFormat : uint16_t
{
    Unknown = 0,
    RGBA8_UNORM,
    BGRA8_UNORM,
    RG16F,
    RGBA16F,
    D24S8,
    D32F
};

struct RHIBufferDesc
{
    uint64_t sizeBytes = 0;
    uint32_t usageMask = 0;
};

struct RHITextureDesc
{
    uint32_t width  = 1;
    uint32_t height = 1;
    uint32_t depth  = 1;
    uint16_t mipLevels   = 1;
    uint16_t arrayLayers = 1;
    uint8_t  sampleCount = 1;
    RHITextureFormat format = RHITextureFormat::Unknown;
    uint32_t usageMask = 0;
};

struct RHIResourceDesc
{
    RHIResourceType type = RHIResourceType::Buffer;
    RHIBufferDesc  buffer;
    RHITextureDesc texture;

    static RHIResourceDesc Buffer(uint64_t sizeBytes, uint32_t usageMask = 0)
    {
        RHIResourceDesc d;
        d.type = RHIResourceType::Buffer;
        d.buffer.sizeBytes = sizeBytes;
        d.buffer.usageMask = usageMask;
        return d;
    }

    static RHIResourceDesc Texture2D(
        uint32_t width, uint32_t height,
        RHITextureFormat format,
        uint16_t mipLevels = 1,
        uint16_t arrayLayers = 1,
        uint8_t sampleCount = 1,
        uint32_t usageMask = 0)
    {
        RHIResourceDesc d;
        d.type = RHIResourceType::Texture;
        d.texture.width = width;
        d.texture.height = height;
        d.texture.depth = 1;
        d.texture.mipLevels = mipLevels;
        d.texture.arrayLayers = arrayLayers;
        d.texture.sampleCount = sampleCount;
        d.texture.format = format;
        d.texture.usageMask = usageMask;
        return d;
    }
};

struct RHIResource
{
    RHIResourceID id = InvalidRHIResource;
    RHIResourceDesc desc{};
    std::string name; // debug name
};
}