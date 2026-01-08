#pragma once
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <Graphic/RenderGraph/RenderGraphIR.h>

namespace Oyi::Graphic
{
class RenderGraphBuilder
{
public:
    RenderGraphBuilder() = default;

    // Create a *logical* resource; returns a handle whose baseId==id.
    RGResourceHandle CreateBufferH(const std::string& name, uint64_t sizeBytes, uint32_t usageMask = 0)
    {
        const RHIResourceID id = NextResourceID();
        RHIResource res;
        res.id = id;
        res.name = name;
        res.desc = RHIResourceDesc::Buffer(sizeBytes, usageMask);
        resources.push_back(res);

        // baseId of itself
        baseOf[id] = id;
        currentVersionOfBase[id] = id;
        versionIndex[id] = 0;

        return RGResourceHandle{ id, id };
    }

    RGResourceHandle CreateTexture2DH(
        const std::string& name,
        uint32_t width, uint32_t height,
        RHITextureFormat format,
        uint16_t mipLevels = 1,
        uint16_t arrayLayers = 1,
        uint8_t sampleCount = 1,
        uint32_t usageMask = 0)
    {
        const RHIResourceID id = NextResourceID();
        RHIResource res;
        res.id = id;
        res.name = name;
        res.desc = RHIResourceDesc::Texture2D(width, height, format, mipLevels, arrayLayers, sampleCount, usageMask);
        resources.push_back(res);

        baseOf[id] = id;
        currentVersionOfBase[id] = id;
        versionIndex[id] = 0;

        return RGResourceHandle{ id, id };
    }

    // Backward compatible API (optional)
    RHIResourceID CreateBuffer(const std::string& name, uint64_t sizeBytes, uint32_t usageMask = 0)
    {
        return CreateBufferH(name, sizeBytes, usageMask).id;
    }
    RHIResourceID CreateTexture2D(const std::string& name, uint32_t w, uint32_t h, RHITextureFormat fmt,
                                 uint16_t mips = 1, uint16_t layers = 1, uint8_t samples = 1, uint32_t usageMask = 0)
    {
        return CreateTexture2DH(name, w, h, fmt, mips, layers, samples, usageMask).id;
    }

    // Create pass
    RGPass& AddPass(const std::string& name)
    {
        RGPass p;
        p.index = static_cast<uint32_t>(passes.size());
        p.name = name;
        passes.push_back(std::move(p));
        return passes.back();
    }

    // Explicit read access: uses handle.id (current version)
    void Read(uint32_t passIndex, const RGResourceHandle& h, uint32_t usageMask = 0, RGSubresourceRange range = {})
    {
        AddAccess(passIndex, RGResourceAccess{ h.id, RGAccessType::Read, usageMask, range });
    }

    // Explicit write access:
    // - produces a NEW version id (SSA)
    // - records a write access to that new version in the specified pass
    // - returns the new handle (same baseId, new id)
    RGResourceHandle Write(uint32_t passIndex, const RGResourceHandle& h, uint32_t usageMask = 0, RGSubresourceRange range = {})
    {
        const RHIResourceID newId = CloneAsNewVersion(h.baseId);

        AddAccess(passIndex, RGResourceAccess{ newId, RGAccessType::Write, usageMask, range });

        // Update current version for this base
        currentVersionOfBase[h.baseId] = newId;

        return RGResourceHandle{ h.baseId, newId };
    }

    // Convenience: get latest version handle of a base (for wiring graph easily)
    RGResourceHandle Latest(RHIResourceID baseId) const
    {
        auto it = currentVersionOfBase.find(baseId);
        if (it == currentVersionOfBase.end()) return RGResourceHandle{};
        return RGResourceHandle{ baseId, it->second };
    }

    // Pass access helper
    void AddAccess(uint32_t passIndex, const RGResourceAccess& access)
    {
        passes.at(passIndex).accesses.push_back(access);
    }

    // Metadata queries for compiler/backend (still API-agnostic)
    RHIResourceID BaseOf(RHIResourceID versionedId) const
    {
        auto it = baseOf.find(versionedId);
        return (it == baseOf.end()) ? InvalidRHIResource : it->second;
    }
    uint32_t VersionIndex(RHIResourceID versionedId) const
    {
        auto it = versionIndex.find(versionedId);
        return (it == versionIndex.end()) ? 0 : it->second;
    }

    const std::vector<RHIResource>& GetResources() const { return resources; }
    const std::vector<RGPass>&      GetPasses() const    { return passes; }

private:
    RHIResourceID NextResourceID()
    {
        return static_cast<RHIResourceID>(resources.size() + 1); // 0 reserved
    }

    RHIResourceID CloneAsNewVersion(RHIResourceID baseId)
    {
        // Find base resource desc/name
        const RHIResource* baseRes = nullptr;
        for (const auto& r : resources)
        {
            if (r.id == baseId) { baseRes = &r; break; }
        }
        // In production, handle error properly.
        if (!baseRes) return InvalidRHIResource;

        const RHIResourceID id = NextResourceID();

        RHIResource res;
        res.id = id;
        res.desc = baseRes->desc;
        res.name = baseRes->name + "_v" + std::to_string(versionIndex[ currentVersionOfBase[baseId] ] + 1);
        resources.push_back(res);

        baseOf[id] = baseId;

        const uint32_t newVer = versionIndex[ currentVersionOfBase[baseId] ] + 1;
        versionIndex[id] = newVer;

        return id;
    }

private:
    std::vector<RHIResource> resources;
    std::vector<RGPass> passes;

    // versioned resource id -> base id
    std::unordered_map<RHIResourceID, RHIResourceID> baseOf;
    // base id -> latest versioned id
    std::unordered_map<RHIResourceID, RHIResourceID> currentVersionOfBase;
    // versioned resource id -> version ordinal
    std::unordered_map<RHIResourceID, uint32_t> versionIndex;
};
}