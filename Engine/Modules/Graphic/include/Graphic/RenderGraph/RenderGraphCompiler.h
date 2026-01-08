#pragma once
#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include <Graphic/RenderGraph/RenderGraphIR.h>

namespace Oyi::Graphic
{
using RGNodeID = uint32_t; // strictly 0..passCount-1

class RenderGraphBuilder;

struct RGCompiledPass
{
    RGNodeID node = 0;             // internal node id
    uint32_t passIndex = 0;         // original builder pass index (debug/stable handle)
    std::string name;
    std::vector<RGResourceAccess> accesses;
};

struct RGResourceLifetime
{
    // Lifetime in *topological order positions* [begin, end]
    uint32_t begin = 0;
    uint32_t end   = 0;
};

struct RGCompiledGraph
{
    // FIFO topological order (stable, predictable).
    std::vector<RGNodeID> topoOrder;

    // Passes aligned with topoOrder.
    std::vector<RGCompiledPass> passesInTopo;

    // Direct dependency list: directDeps[dst] contains the set of immediate prerequisites of dst.
    // i.e. for each edge (src -> dst), src is in directDeps[dst].
    std::vector<std::vector<RGNodeID>> directDeps;

    // Optional helper: adjacency (outgoing edges) can also be useful for backends.
    // If you don't want to expose it, you can omit it.
    std::vector<std::vector<RGNodeID>> directSuccs;

    // Virtual resource id -> [begin,end] in topo order.
    std::unordered_map<RHIResourceID, RGResourceLifetime> lifetimes;

    // Virtual resource id -> physical resource id (alias target). If absent, physical==virtual.
    std::unordered_map<RHIResourceID, RHIResourceID> aliasMap;

    // Query helpers (safe bounds assumed by caller).
    bool HasDirectDep(RGNodeID dst, RGNodeID src) const
    {
        const auto& deps = directDeps[dst];
        return std::find(deps.begin(), deps.end(), src) != deps.end();
    }
};

class RenderGraphCompiler
{
public:
    void Compile(const RenderGraphBuilder& builder);

    const RGCompiledGraph& GetCompiledGraph() const { return compiled; }

private:
    void BuildDependencies();
    void TopologicalSortFIFO();
    void AnalyzeResourceLifetimes();
    void AssignAliasing();

    void AddEdge(RGNodeID from, RGNodeID to);
    uint64_t MakeCompatKey(const RHIResourceDesc& desc) const;

private:
    // Inputs (copied for compilation stability)
    std::vector<RHIResource> resources;
    std::vector<RGPass> passes; // node id == index in this vector

    // Graph internal
    std::vector<std::vector<RGNodeID>> adjacency; // outgoing edges
    std::vector<uint32_t> inDegree;

    // Output
    RGCompiledGraph compiled{};
};
}