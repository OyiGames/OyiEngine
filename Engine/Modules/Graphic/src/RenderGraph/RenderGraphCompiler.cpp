#include <algorithm>
#include <cassert>
#include <deque>

#include <Graphic/RenderGraph/RenderGraphBuilder.h>
#include <Graphic/RenderGraph/RenderGraphCompiler.h>

using namespace Oyi::Graphic;

void RenderGraphCompiler::Compile(const RenderGraphBuilder& builder)
{
    resources = builder.GetResources();
    passes    = builder.GetPasses();

    adjacency.assign(passes.size(), {});
    inDegree.assign(passes.size(), 0);

    compiled = RGCompiledGraph{};
    compiled.directDeps.assign(passes.size(), {});
    compiled.directSuccs.assign(passes.size(), {});

    BuildDependencies();
    TopologicalSortFIFO();
    AnalyzeResourceLifetimes();
    AssignAliasing();

    // Build compiled passes in topo order
    compiled.passesInTopo.clear();
    compiled.passesInTopo.reserve(compiled.topoOrder.size());

    for (uint32_t topoPos = 0; topoPos < static_cast<uint32_t>(compiled.topoOrder.size()); ++topoPos)
    {
        const RGNodeID node = compiled.topoOrder[topoPos];
        const RGPass&  pass = passes.at(node);

        RGCompiledPass out;
        out.node = node;
        out.passIndex = pass.index;
        out.name = pass.name;
        out.accesses = pass.accesses;

        compiled.passesInTopo.push_back(std::move(out));
    }

    // Expose direct successors too (optional but handy).
    compiled.directSuccs = adjacency;
}

void RenderGraphCompiler::AddEdge(RGNodeID from, RGNodeID to)
{
    if (from == to) return;

    // De-dup edges (avoid inflating indegrees).
    auto& out = adjacency[from];
    if (std::find(out.begin(), out.end(), to) != out.end())
        return;

    out.push_back(to);
    inDegree[to] += 1;

    // Record direct deps (incoming edge list) for output.
    auto& deps = compiled.directDeps[to];
    deps.push_back(from);
}

void RenderGraphCompiler::BuildDependencies()
{
    // Hazard tracking per virtual resource.
    std::unordered_map<RHIResourceID, RGNodeID> lastWriter;
    std::unordered_map<RHIResourceID, std::vector<RGNodeID>> lastReaders;

    for (RGNodeID node = 0; node < static_cast<RGNodeID>(passes.size()); ++node)
    {
        const RGPass& pass = passes[node];

        for (const RGResourceAccess& a : pass.accesses)
        {
            if (a.resource == InvalidRHIResource)
                continue;

            const auto wIt = lastWriter.find(a.resource);

            if (a.access == RGAccessType::Read)
            {
                // RAW: last writer -> this reader
                if (wIt != lastWriter.end())
                    AddEdge(wIt->second, node);

                lastReaders[a.resource].push_back(node);
            }
            else // Write
            {
                // WAW: last writer -> this writer
                if (wIt != lastWriter.end())
                    AddEdge(wIt->second, node);

                // WAR: all last readers -> this writer
                auto rIt = lastReaders.find(a.resource);
                if (rIt != lastReaders.end())
                {
                    for (RGNodeID reader : rIt->second)
                        AddEdge(reader, node);
                    rIt->second.clear();
                }

                lastWriter[a.resource] = node;
            }
        }
    }

    // Optional: sort directDeps for deterministic output (nice for debugging).
    for (auto& deps : compiled.directDeps)
    {
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
    }
}

void RenderGraphCompiler::TopologicalSortFIFO()
{
    compiled.topoOrder.clear();
    compiled.topoOrder.reserve(passes.size());

    std::deque<RGNodeID> q;
    std::vector<uint32_t> indeg = inDegree;

    // FIFO: initial scan in node id order guarantees stability.
    for (RGNodeID n = 0; n < static_cast<RGNodeID>(passes.size()); ++n)
    {
        if (indeg[n] == 0)
            q.push_back(n);
    }

    while (!q.empty())
    {
        const RGNodeID n = q.front();
        q.pop_front();

        compiled.topoOrder.push_back(n);

        // Note: adjacency order influences FIFO behavior.
        // You can sort adjacency[n] once if you want deterministic unlock order.
        for (RGNodeID m : adjacency[n])
        {
            assert(indeg[m] > 0);
            indeg[m] -= 1;
            if (indeg[m] == 0)
                q.push_back(m);
        }
    }

    // Cycle fallback: keep original order (or assert/report cycle in production).
    if (compiled.topoOrder.size() != passes.size())
    {
        compiled.topoOrder.clear();
        compiled.topoOrder.reserve(passes.size());
        for (RGNodeID n = 0; n < static_cast<RGNodeID>(passes.size()); ++n)
            compiled.topoOrder.push_back(n);
    }
}

void RenderGraphCompiler::AnalyzeResourceLifetimes()
{
    compiled.lifetimes.clear();

    // Map node -> topo position
    std::vector<uint32_t> topoPos(passes.size(), 0);
    for (uint32_t i = 0; i < static_cast<uint32_t>(compiled.topoOrder.size()); ++i)
        topoPos[compiled.topoOrder[i]] = i;

    for (RGNodeID node = 0; node < static_cast<RGNodeID>(passes.size()); ++node)
    {
        const uint32_t pos = topoPos[node];
        const RGPass& pass = passes[node];

        for (const RGResourceAccess& a : pass.accesses)
        {
            if (a.resource == InvalidRHIResource)
                continue;

            auto it = compiled.lifetimes.find(a.resource);
            if (it == compiled.lifetimes.end())
            {
                compiled.lifetimes.emplace(a.resource, RGResourceLifetime{pos, pos});
            }
            else
            {
                it->second.begin = std::min(it->second.begin, pos);
                it->second.end   = std::max(it->second.end, pos);
            }
        }
    }
}

uint64_t RenderGraphCompiler::MakeCompatKey(const RHIResourceDesc& desc) const
{
    uint64_t key = 1469598103934665603ull; // FNV offset
    auto fnv1a = [&](uint64_t v)
    {
        key ^= v;
        key *= 1099511628211ull;
    };

    fnv1a(static_cast<uint64_t>(desc.type));
    if (desc.type == RHIResourceType::Buffer)
    {
        fnv1a(desc.buffer.sizeBytes);
        fnv1a(desc.buffer.usageMask);
    }
    else
    {
        fnv1a(desc.texture.width);
        fnv1a(desc.texture.height);
        fnv1a(desc.texture.depth);
        fnv1a(desc.texture.mipLevels);
        fnv1a(desc.texture.arrayLayers);
        fnv1a(desc.texture.sampleCount);
        fnv1a(static_cast<uint64_t>(desc.texture.format));
        fnv1a(desc.texture.usageMask);
    }

    return key;
}

void RenderGraphCompiler::AssignAliasing()
{
    compiled.aliasMap.clear();

    // Build a lookup resourceId -> desc
    std::unordered_map<RHIResourceID, const RHIResourceDesc*> descOf;
    descOf.reserve(resources.size());
    for (const RHIResource& r : resources)
        descOf[r.id] = &r.desc;

    // Bucket virtual resources by compatibility key
    struct ResInfo
    {
        RHIResourceID id;
        uint32_t begin;
        uint32_t end;
    };

    std::unordered_map<uint64_t, std::vector<ResInfo>> buckets;
    buckets.reserve(resources.size());

    for (const auto& [rid, lt] : compiled.lifetimes)
    {
        auto dIt = descOf.find(rid);
        if (dIt == descOf.end())
            continue;

        const uint64_t key = MakeCompatKey(*dIt->second);
        buckets[key].push_back(ResInfo{rid, lt.begin, lt.end});
    }

    // For each bucket, do interval reuse scan.
    for (auto& [key, vec] : buckets)
    {
        (void)key;
        std::sort(vec.begin(), vec.end(), [](const ResInfo& a, const ResInfo& b)
        {
            if (a.begin != b.begin) return a.begin < b.begin;
            return a.end < b.end;
        });

        struct Active
        {
            RHIResourceID physical;
            uint32_t end;
        };
        std::vector<Active> active;

        for (const ResInfo& r : vec)
        {
            auto reuseIt = std::find_if(active.begin(), active.end(),
                [&](const Active& a) { return a.end < r.begin; });

            if (reuseIt != active.end())
            {
                compiled.aliasMap[r.id] = reuseIt->physical;
                reuseIt->end = r.end;
            }
            else
            {
                active.push_back(Active{r.id, r.end});
            }
        }
    }
}