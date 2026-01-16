#include <algorithm>
#include <assert.h>
#include <deque>
#include <sstream>
#include <fmt/base.h>
#include <Graphic/RenderGraph/RenderGraphBuilder.h>
#include <Graphic/RenderGraph/RenderGraphCompiler.h>

using namespace Oyi::Graphic;

void RenderGraphCompiler::Compile(const RenderGraphBuilder& b)
{
    resources = b.GetResources();
    passes    = b.GetPasses();
    builder   = &b;

    adjacency.assign(passes.size(), {});
    inDegree.assign(passes.size(), 0);

    compiled = RGCompiledGraph{};
    compiled.directDeps.assign(passes.size(), {});
    compiled.directSuccs.assign(passes.size(), {});

    Validate();
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

void RenderGraphCompiler::Validate() const
{
    if (!builder)
        fmt::print(stderr, "RenderGraphCompiler::Validate: builder is null!\n");

    // ------------------ Resource lookup ------------------
    std::unordered_map<RHIResourceID, const RHIResourceDesc*> descOf;
    descOf.reserve(resources.size());
    for (const auto& r : resources)
        descOf[r.id] = &r.desc;

    auto HasRes = [&](RHIResourceID id) -> bool {
        return id != InvalidRHIResource && descOf.find(id) != descOf.end();
    };

    // ------------------ Pass index sanity (node id must match pass.index) ------------------
    for (RGNodeID node = 0; node < static_cast<RGNodeID>(passes.size()); ++node)
    {
        const auto& p = passes[node];
        if (p.index != node)
        {
            fmt::print(stderr, "pass.index mismatch: pass index mismatch: node={} pass.index={} pass='{}'\n",
                       node, p.index, p.name);
        }
    }

    // ------------------ Version table (base -> versionIndex -> versionedId) ------------------
    std::unordered_map<RHIResourceID, std::unordered_map<uint32_t, RHIResourceID>> versionsByBase;
    versionsByBase.reserve(resources.size());

    for (const auto& r : resources)
    {
        const RHIResourceID vid  = r.id;
        const RHIResourceID base = builder->BaseOf(vid);
        const uint32_t ver       = builder->VersionIndex(vid);

        if (base == InvalidRHIResource)
            fmt::print(stderr, "resource id={} has invalid baseId.", vid);

        if (!HasRes(base))
            fmt::print(stderr, "resource id={} has baseId={} which is not present in resources table.", vid, base);

        auto& m = versionsByBase[base];
        auto it = m.find(ver);
        if (it != m.end() && it->second != vid)
            fmt::print(stderr, "duplicate version index for same base: base={} ver={} maps to {} and {}.",
                      base, ver, it->second, vid);

        m[ver] = vid;
    }

    // ------------------ Per-version use stats ------------------
    struct UseStat
    {
        bool used = false;
        RGNodeID firstUse = 0;
        RGNodeID lastUse  = 0;

        uint32_t writerCount = 0;
        bool hasWriter = false;
        RGNodeID writer = 0;

        uint32_t readCount = 0;
        bool hasReader = false;
    };

    std::unordered_map<RHIResourceID, UseStat> stat;
    stat.reserve(passes.size() * 4);

    // pass -> set of read resources / written resources
    std::unordered_map<RGNodeID, std::unordered_set<RHIResourceID>> readsByPass;
    std::unordered_map<RGNodeID, std::unordered_set<RHIResourceID>> writesByPass;
    readsByPass.reserve(passes.size());
    writesByPass.reserve(passes.size());

    // ------------------ Scan passes: references + empty pass + subresource bounds ------------------
    for (RGNodeID node = 0; node < static_cast<RGNodeID>(passes.size()); ++node)
    {
        const auto& p = passes[node];

        bool hasAnyRGAccess = false;

        // detect duplicate writes inside a single pass access list
        std::unordered_set<RHIResourceID> writesInPass;

        for (const auto& a : p.accesses)
        {
            if (a.resource == InvalidRHIResource)
                continue;

            hasAnyRGAccess = true;

            if (!HasRes(a.resource))
                fmt::print(stderr, "pass '{}' references unknown resource id={}.", p.name, a.resource);

            // usage stats
            auto& s = stat[a.resource];
            if (!s.used)
            {
                s.used = true;
                s.firstUse = node;
                s.lastUse = node;
            }
            else
            {
                s.lastUse = node;
            }

            if (a.access == RGAccessType::Read)
            {
                s.hasReader = true;
                s.readCount++;
                readsByPass[node].insert(a.resource);
            }
            else
            {
                if (!writesInPass.insert(a.resource).second)
                    fmt::print(stderr, "pass '{}' writes the same resource id={} multiple times in its access list.",
                              p.name, a.resource);

                s.hasWriter = true;
                s.writer = node;
                s.writerCount++;
                writesByPass[node].insert(a.resource);
            }

            // Subresource bounds (texture only)
            const RHIResourceDesc& d = *descOf.at(a.resource);
            if (d.type == RHIResourceType::Texture)
            {
                if (a.range.mipCount != 0)
                {
                    const uint32_t endMip = static_cast<uint32_t>(a.range.baseMip) +
                                            static_cast<uint32_t>(a.range.mipCount);
                    if (endMip > d.texture.mipLevels)
                    {
                        fmt::print(stderr, "pass '{}' uses texture id={} mip range out of bounds: baseMip={} mipCount={} mipLevels={}.",
                                  p.name, a.resource, a.range.baseMip, a.range.mipCount, d.texture.mipLevels);
                    }
                }
                if (a.range.layerCount != 0)
                {
                    const uint32_t endLayer = static_cast<uint32_t>(a.range.baseLayer) +
                                              static_cast<uint32_t>(a.range.layerCount);
                    if (endLayer > d.texture.arrayLayers)
                    {
                        fmt::print(stderr, "pass '{}' uses texture id={} layer range out of bounds: baseLayer={} layerCount={} arrayLayers={}.",
                                  p.name, a.resource, a.range.baseLayer, a.range.layerCount, d.texture.arrayLayers);
                    }
                }
            }
        }

        // Empty pass is always wrong in RDG (no observable dependencies or outputs)
        if (!hasAnyRGAccess)
            fmt::print(stderr, "pass '{}' has no RDG resource accesses (no read/write). Remove it or add explicit accesses.", p.name);

        // Read-only pass is allowed, but warn (optional)
        const bool hasWrite = !writesByPass[node].empty();
        const bool hasRead  = !readsByPass[node].empty();
        if (!hasWrite && hasRead)
            fmt::print(stderr, "pass '{}' is read-only (no RDG writes). Allowed, but verify it has real side effects.", p.name);
    }

    // ------------------ SSA: writer count rules ------------------
    // Policy:
    // - ver==0: allow 0 or 1 writer. >1 is fatal.
    // - ver>0: must be written exactly once.
    for (const auto& r : resources)
    {
        const RHIResourceID vid = r.id;
        const uint32_t ver = builder->VersionIndex(vid);

        const auto it = stat.find(vid);
        const uint32_t wc = (it == stat.end()) ? 0u : it->second.writerCount;

        if (ver == 0)
        {
            if (wc > 1)
                fmt::print(stderr, "SSA violation: v0 resource id={} has {} writers (expected 0 or 1).", vid, wc);
        }
        else
        {
            if (wc != 1)
                fmt::print(stderr, "SSA violation: resource id={} (ver={}) must have exactly 1 writer, but has {}.",
                          vid, ver, wc);
        }
    }

    // ------------------ Version holes (base must have 0..max) ------------------
    for (const auto& [base, m] : versionsByBase)
    {
        uint32_t maxV = 0;
        for (const auto& kv : m) maxV = std::max(maxV, kv.first);

        for (uint32_t v = 0; v <= maxV; ++v)
        {
            if (m.find(v) == m.end())
                fmt::print(stderr, "version hole detected: base={} missing v{} but higher version(s) exist up to v{}.",
                          base, v, maxV);
        }
    }

    // ------------------ Derive vs Overwrite classification (warn only) ------------------
    // writer(Vk) reads Vk-1 => Derive. Otherwise Overwrite (allowed; warn).
    for (const auto& r : resources)
    {
        const RHIResourceID vid  = r.id;
        const RHIResourceID base = builder->BaseOf(vid);
        const uint32_t ver       = builder->VersionIndex(vid);

        if (ver == 0) continue;

        const auto it = stat.find(vid);
        if (it == stat.end() || it->second.writerCount != 1)
            continue;

        const RGNodeID w = it->second.writer;

        const auto baseIt = versionsByBase.find(base);
        if (baseIt == versionsByBase.end())
            fmt::print(stderr, "internal error: base={} not found in versionsByBase.", base);

        const auto prevIt = baseIt->second.find(ver - 1);
        if (prevIt == baseIt->second.end())
            fmt::print(stderr, "internal error: base={} missing previous version v{}.", base, ver - 1);

        const RHIResourceID vPrev = prevIt->second;

        const auto rpIt = readsByPass.find(w);
        const bool readsPrev = (rpIt != readsByPass.end() &&
                                rpIt->second.find(vPrev) != rpIt->second.end());

        if (!readsPrev)
        {
            fmt::print(stderr, "overwrite write: pass '{}' (node={}) writes id={} (ver={}) without reading previous id={} (ver={}).",
                     passes[w].name, w, vid, ver, vPrev, ver - 1);
        }
    }
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
    // Hazard tracking on *versioned* resource IDs (your existing behavior).
    std::unordered_map<RHIResourceID, RGNodeID> lastWriter;
    std::unordered_map<RHIResourceID, std::vector<RGNodeID>> lastReaders;

    // Stats per versioned resource, used to build version-chain edges robustly.
    struct Stat
    {
        bool hasUse = false;
        RGNodeID firstUse = 0;
        RGNodeID lastUse  = 0;

        bool hasWriter = false;
        RGNodeID writer = 0; // last writer encountered (should be unique in SSA)
    };
    std::unordered_map<RHIResourceID, Stat> stat;
    stat.reserve(passes.size() * 4);

    // Build RAW/WAR/WAW + gather per-version use ranges.
    for (RGNodeID node = 0; node < static_cast<RGNodeID>(passes.size()); ++node)
    {
        const RGPass& pass = passes[node];

        for (const RGResourceAccess& a : pass.accesses)
        {
            if (a.resource == InvalidRHIResource)
                continue;

            // ---- gather stats ----
            auto& s = stat[a.resource];
            if (!s.hasUse)
            {
                s.hasUse = true;
                s.firstUse = node;
                s.lastUse  = node;
            }
            else
            {
                s.lastUse = node;
            }

            if (a.access == RGAccessType::Write)
            {
                if (!s.hasWriter)
                {
                    s.hasWriter = true;
                    s.writer = node;
                }
                else
                {
                    s.writer = node;
                }
            }

            // ---- hazard edges on versioned id ----
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

    // ---------------- Version-chain edges (base resource order) ----------------
    // Require builder to provide BaseOf/VersionIndex (SSA metadata).
    // If builder is null, we cannot enforce version order (would be a bug in setup).
    if (!builder) return;

    // base -> versions[k] = versionedId
    std::unordered_map<RHIResourceID, std::vector<RHIResourceID>> versions;
    versions.reserve(stat.size());

    // Use all versioned IDs that actually appear in passes (stat keys),
    // so pass insertion order doesn't matter.
    for (const auto& kv : stat)
    {
        const RHIResourceID vid  = kv.first;
        const RHIResourceID base = builder->BaseOf(vid);
        if (base == InvalidRHIResource) continue;

        const uint32_t k = builder->VersionIndex(vid);

        auto& vec = versions[base];
        if (vec.size() <= k) vec.resize(k + 1, InvalidRHIResource);
        vec[k] = vid;
    }

    // Add edges for every base: for k=1..N:
    //   lastUse(Vk-1) -> writer(Vk)   (critical)
    //   writer(Vk-1)  -> writer(Vk)   (stabilize write chain)
    //   writer(Vk)    -> firstUse(Vk) (optional safety, often redundant)
    for (auto& [base, vec] : versions)
    {
        (void)base;

        for (uint32_t k = 1; k < static_cast<uint32_t>(vec.size()); ++k)
        {
            const RHIResourceID vPrev = vec[k - 1];
            const RHIResourceID vCur  = vec[k];

            if (vPrev == InvalidRHIResource || vCur == InvalidRHIResource)
                continue;

            auto itPrev = stat.find(vPrev);
            auto itCur  = stat.find(vCur);

            // If current version is never written, there's no "producer" to constrain.
            if (itCur == stat.end() || !itCur->second.hasWriter)
                continue;

            // (A) Ensure producing vCur cannot happen before finishing any use of vPrev.
            if (itPrev != stat.end() && itPrev->second.hasUse)
                AddEdge(itPrev->second.lastUse, itCur->second.writer);

            // (B) Stabilize writer chain (good practice).
            if (itPrev != stat.end() && itPrev->second.hasWriter)
                AddEdge(itPrev->second.writer, itCur->second.writer);

            // (C) Optional: ensure uses of vCur occur after its writer (RAW usually already covers reads,
            // but this helps if some passes only "use" vCur implicitly).
            if (itCur->second.hasUse)
                AddEdge(itCur->second.writer, itCur->second.firstUse);
        }
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