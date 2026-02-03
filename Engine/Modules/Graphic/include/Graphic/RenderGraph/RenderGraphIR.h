#pragma once

// RenderGraph intermediate representation (IR) types.
//
// Design goals:
// - Backend-agnostic: no Vulkan/D3D12-specific fields.
// - Minimal dependencies: intended to be included by Builder, Compiler, and Backends.
// - Stable ABI surface: avoid pulling in Builder/Compiler implementation details.

#include <cstdint>
#include <string>
#include <vector>

namespace Oyi::Graphic
{
using RHIResourceID = uint32_t;
static constexpr RHIResourceID InvalidRHIResource = 0;

// Backend-agnostic access intent. Backends map this to API states/barriers.
enum class RGAccessType : uint8_t
{
    Read,
    Write
};

// Minimal subresource range abstraction.
struct RGSubresourceRange
{
    uint16_t baseMip    = 0;
    uint16_t mipCount   = 0; // 0 == all
    uint16_t baseLayer  = 0;
    uint16_t layerCount = 0; // 0 == all
};

// A single resource access inside a pass.
//
// Note: resource is expected to be a *versioned virtual id* (SSA form).
// Backends should not interpret this as a physical allocation id.
struct RGResourceAccess
{
    RHIResourceID resource = InvalidRHIResource;
    RGAccessType  access   = RGAccessType::Read;

    // Abstract usage class (bitmask) for backend interpretation.
    // Example usage: shader read, color attachment, depth attachment, transfer src/dst.
    uint32_t usageMask = 0;

    RGSubresourceRange range{};
};

// A render graph node (pass) in the IR.
struct RGPass
{
    uint32_t index = 0; // contiguous; compiler uses it as node id
    std::string name;
    std::vector<RGResourceAccess> accesses;
};

// ---------------- SSA resource handle ----------------
// baseId: the logical resource identity (stable).
// id:     the current version id used for reads/writes in the graph.
struct RGResourceHandle
{
    RHIResourceID baseId = InvalidRHIResource;
    RHIResourceID id     = InvalidRHIResource;

    explicit operator bool() const { return id != InvalidRHIResource; }
};
} // namespace Oyi::Graphic
