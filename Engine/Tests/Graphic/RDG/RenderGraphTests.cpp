#include <gtest/gtest.h>

// Prefer including IR explicitly in tests (stable surface).
#include <Graphic/RenderGraph/RenderGraphIR.h>
#include <Graphic/RenderGraph/RenderGraphBuilder.h>
#include <Graphic/RenderGraph/RenderGraphCompiler.h>

using namespace Oyi::Graphic;

namespace
{
static uint32_t FindTopoPos(const RGCompiledGraph& g, RGNodeID node)
{
    for (uint32_t i = 0; i < static_cast<uint32_t>(g.topoOrder.size()); ++i)
        if (g.topoOrder[i] == node) return i;
    return 0xFFFFFFFFu;
}
} // namespace

TEST(RenderGraphBuilder, SSA_WriteCreatesNewVersionAndUpdatesLatest)
{
    RenderGraphBuilder b;
    auto buf = b.CreateBufferH("buf", 1024);

    // baseId == id for first version
    EXPECT_EQ(buf.baseId, buf.id);
    EXPECT_EQ(b.BaseOf(buf.id), buf.baseId);
    EXPECT_EQ(b.VersionIndex(buf.id), 0u);

    b.AddPass("P0");

    // Write should create a NEW version id
    auto v1 = b.Write(0, buf);
    EXPECT_EQ(v1.baseId, buf.baseId);
    EXPECT_NE(v1.id, buf.id);
    EXPECT_EQ(b.BaseOf(v1.id), buf.baseId);
    EXPECT_EQ(b.VersionIndex(v1.id), 1u);

    // Latest should return the newest version
    auto latest = b.Latest(buf.baseId);
    EXPECT_EQ(latest.id, v1.id);

    // The pass should contain a write access to v1
    const auto& passes = b.GetPasses();
    ASSERT_EQ(passes.size(), 1u);
    ASSERT_EQ(passes[0].accesses.size(), 1u);

    EXPECT_EQ(passes[0].accesses[0].resource, v1.id);
    EXPECT_EQ(passes[0].accesses[0].access, RGAccessType::Write);
}

TEST(RenderGraphCompiler, BuildsRAWDependenciesOnSameVersionedResource)
{
    RenderGraphBuilder b;
    auto tex = b.CreateTexture2DH("tex", 64, 64, RHITextureFormat::RGBA8_UNORM);

    // Pass A writes v1
    b.AddPass("A");
    auto v1 = b.Write(0, tex);

    // Pass B reads v1 => A -> B must exist
    b.AddPass("B");
    b.Read(1, v1);

    RenderGraphCompiler c;
    c.Compile(b);
    const auto& g = c.GetCompiledGraph();

    ASSERT_EQ(g.directDeps.size(), 2u);
    EXPECT_TRUE(g.HasDirectDep(/*dst*/ 1, /*src*/ 0));

    // And topo order must schedule A before B
    const uint32_t posA = FindTopoPos(g, 0);
    const uint32_t posB = FindTopoPos(g, 1);
    EXPECT_LT(posA, posB);
}

TEST(RenderGraphCompiler, SSAVersionsDoNotCreateFalseDependenciesAcrossBaseResource)
{
    RenderGraphBuilder b;
    auto buf = b.CreateBufferH("buf", 256);

    // Pass A writes v1
    b.AddPass("A");
    auto v1 = b.Write(0, buf);

    // Pass B reads v0 (original id), not v1.
    // In SSA model, these are distinct versioned ids => no edge required (given current compiler rules).
    b.AddPass("B");
    b.Read(1, buf);

    RenderGraphCompiler c;
    c.Compile(b);
    const auto& g = c.GetCompiledGraph();

    EXPECT_FALSE(g.HasDirectDep(/*dst*/ 1, /*src*/ 0));

    // If independent, FIFO topo should remain stable (0 then 1)
    ASSERT_EQ(g.topoOrder.size(), 2u);
    EXPECT_EQ(g.topoOrder[0], 0u);
    EXPECT_EQ(g.topoOrder[1], 1u);

    (void)v1;
}

TEST(RenderGraphCompiler, FIFO_TopologicalSortIsStableForIndependentPasses)
{
    RenderGraphBuilder b;
    b.AddPass("P0");
    b.AddPass("P1");
    b.AddPass("P2");

    RenderGraphCompiler c;
    c.Compile(b);
    const auto& g = c.GetCompiledGraph();

    ASSERT_EQ(g.topoOrder.size(), 3u);
    EXPECT_EQ(g.topoOrder[0], 0u);
    EXPECT_EQ(g.topoOrder[1], 1u);
    EXPECT_EQ(g.topoOrder[2], 2u);
}

TEST(RenderGraphCompiler, ResourceLifetimeMatchesFirstAndLastTopoUse)
{
    RenderGraphBuilder b;
    auto buf = b.CreateBufferH("buf", 512);

    // P0 uses buf(v0)
    b.AddPass("P0");
    b.Read(0, buf);

    // P1 does not touch buf
    b.AddPass("P1");

    // P2 uses buf(v0) again
    b.AddPass("P2");
    b.Read(2, buf);

    RenderGraphCompiler c;
    c.Compile(b);
    const auto& g = c.GetCompiledGraph();

    auto it = g.lifetimes.find(buf.id);
    ASSERT_TRUE(it != g.lifetimes.end());

    // With FIFO topo positions [0,1,2], lifetime should be [0,2]
    EXPECT_EQ(it->second.begin, 0u);
    EXPECT_EQ(it->second.end, 2u);
}

TEST(RenderGraphCompiler, AliasingReusesCompatibleResourcesWithNonOverlappingLifetimes)
{
    RenderGraphBuilder b;

    // Two compatible buffers, used in disjoint topo positions.
    auto a = b.CreateBufferH("A", 1024);
    auto d = b.CreateBufferH("D", 1024);

    // P0 uses A
    b.AddPass("P0");
    b.Read(0, a);

    // P1 does nothing (gap)
    b.AddPass("P1");

    // P2 uses D
    b.AddPass("P2");
    b.Read(2, d);

    RenderGraphCompiler c;
    c.Compile(b);
    const auto& g = c.GetCompiledGraph();

    ASSERT_TRUE(g.lifetimes.find(a.id) != g.lifetimes.end());
    ASSERT_TRUE(g.lifetimes.find(d.id) != g.lifetimes.end());

    // Expect D aliases to A (first available physical in compat bucket),
    // given non-overlapping lifetimes and identical desc.
    auto it = g.aliasMap.find(d.id);
    ASSERT_TRUE(it != g.aliasMap.end());
    EXPECT_EQ(it->second, a.id);
}
