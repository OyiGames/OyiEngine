#include <fmt/base.h>
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

    // Pass B reads v0
    b.AddPass("B");
    b.Read(1, buf);

    RenderGraphCompiler c;
    c.Compile(b);
    const auto& g = c.GetCompiledGraph();

    EXPECT_FALSE(g.HasDirectDep(/*dst*/ 1, /*src*/ 0));

    // If independent, FIFO topo should remain stable (0 then 1)
    ASSERT_EQ(g.topoOrder.size(), 2u);
    EXPECT_EQ(g.topoOrder[0], 1u);
    EXPECT_EQ(g.topoOrder[1], 0u);

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

TEST(RenderGraphCompiler, SimulatesGameRenderingPipelineDependenciesAndAliasing)
{
    RenderGraphBuilder b;

    auto cameraCB   = b.CreateBufferH("CameraCB", 256);
    auto lightList  = b.CreateBufferH("LightList", 1024);

    auto shadowMap  = b.CreateTexture2DH("ShadowMap", 2048, 2048, RHITextureFormat::D32F);
    auto gAlbedo    = b.CreateTexture2DH("GBufferAlbedo", 1280, 720, RHITextureFormat::RGBA8_UNORM);
    auto gNormal    = b.CreateTexture2DH("GBufferNormal", 1280, 720, RHITextureFormat::RG16F);
    auto gDepth     = b.CreateTexture2DH("GBufferDepth", 1280, 720, RHITextureFormat::D24S8);
    auto lighting   = b.CreateTexture2DH("Lighting", 1280, 720, RHITextureFormat::RG16F);
    auto depthPyramid = b.CreateTexture2DH("DepthPyramid", 640, 360, RHITextureFormat::RG16F);
    auto ssao       = b.CreateTexture2DH("SSAO", 1280, 720, RHITextureFormat::RG16F);
    auto ssr        = b.CreateTexture2DH("SSR", 1280, 720, RHITextureFormat::RGBA16F);
    auto motionVec  = b.CreateTexture2DH("MotionVectors", 1280, 720, RHITextureFormat::RG16F);
    auto taaHistory = b.CreateTexture2DH("TAAHistory", 1280, 720, RHITextureFormat::RGBA16F);
    auto bloomA     = b.CreateTexture2DH("BloomA", 1280, 720, RHITextureFormat::RGBA16F);
    auto bloomB     = b.CreateTexture2DH("BloomB", 1280, 720, RHITextureFormat::RGBA16F);
    auto colorGrade = b.CreateTexture2DH("ColorGradeTemp", 1280, 720, RHITextureFormat::RGBA16F);
    auto backbuffer = b.CreateTexture2DH("Backbuffer", 1280, 720, RHITextureFormat::BGRA8_UNORM);
    auto skinInput  = b.CreateBufferH("SkinInput", 2048);
    auto skinOutput = b.CreateBufferH("SkinOutput", 2048);
    auto particleState = b.CreateBufferH("ParticleState", 4096);
    auto cullingInput = b.CreateBufferH("CullingInput", 1024);
    auto cullingOutput = b.CreateBufferH("CullingOutput", 1024);

    // Shadow: render scene from light view into depth map.
    b.AddPass("Shadow");
    b.Read(0, cameraCB);
    auto shadowV1 = b.Write(0, shadowMap);

    // GBuffer: fill material + normal + depth for deferred shading.
    b.AddPass("GBuffer");
    b.Read(1, cameraCB);
    auto gAlbedoV1 = b.Write(1, gAlbedo);
    auto gNormalV1 = b.Write(1, gNormal);
    auto gDepthV1  = b.Write(1, gDepth);

    // Lighting: combine GBuffer + lights + shadows into a lighting buffer.
    b.AddPass("Lighting");
    b.Read(2, gAlbedoV1);
    b.Read(2, gNormalV1);
    b.Read(2, gDepthV1);
    b.Read(2, shadowV1);
    b.Read(2, lightList);
    auto lightingV1 = b.Write(2, lighting);

    // LightingResolve: read-modify-write lighting (e.g., apply light probes or decals).
    b.AddPass("LightingResolve");
    b.Read(3, lightingV1);
    auto lightingV2 = b.Write(3, lighting);

    // DepthPyramid: downsample depth for culling/occlusion queries.
    b.AddPass("DepthPyramid");
    b.Read(4, gDepthV1);
    auto depthPyramidV1 = b.Write(4, depthPyramid);

    // SSAO: screen-space ambient occlusion from depth pyramid.
    b.AddPass("SSAO");
    b.Read(5, depthPyramidV1);
    auto ssaoV1 = b.Write(5, ssao);

    // SSR: screen-space reflections using normals + depth + lighting.
    b.AddPass("SSR");
    b.Read(6, gNormalV1);
    b.Read(6, gDepthV1);
    b.Read(6, lightingV2);
    auto ssrV1 = b.Write(6, ssr);

    // MotionVectors: per-pixel velocity for TAA and reprojection.
    b.AddPass("MotionVectors");
    b.Read(7, gDepthV1);
    b.Read(7, cameraCB);
    auto motionVecV1 = b.Write(7, motionVec);

    // BloomSetup: extract bright areas from lighting.
    b.AddPass("BloomSetup");
    b.Read(8, lightingV2);
    auto bloomAV1 = b.Write(8, bloomA);

    // BloomBlur: blur the bright pass.
    b.AddPass("BloomBlur");
    b.Read(9, bloomAV1);
    auto bloomBV1 = b.Write(9, bloomB);

    // ToneMap: combine lighting + bloom + AO/SSR into display-referred color.
    b.AddPass("ToneMap");
    b.Read(10, lightingV2);
    b.Read(10, bloomBV1);
    b.Read(10, ssaoV1);
    b.Read(10, ssrV1);
    auto backbufferV1 = b.Write(10, backbuffer);

    // ColorGrade: apply LUT or grading curve.
    b.AddPass("ColorGrade");
    b.Read(11, backbufferV1);
    auto colorGradeV1 = b.Write(11, colorGrade);

    // TAA: temporal anti-aliasing using motion vectors and history.
    b.AddPass("TAA");
    b.Read(12, colorGradeV1);
    b.Read(12, motionVecV1);
    b.Read(12, taaHistory);
    auto backbufferV2 = b.Write(12, backbuffer);

    // UI: render HUD overlays on top of the final color buffer.
    b.AddPass("UI");
    b.Read(13, backbufferV2);
    auto backbufferV3 = b.Write(13, backbuffer);

    // AsyncSkinning: compute skinning on GPU, independent from main graph.
    b.AddPass("AsyncSkinning");
    b.Read(14, skinInput);
    b.Write(14, skinOutput);

    // ParticleSim: simulate particles (independent compute).
    b.AddPass("ParticleSim");
    b.Read(15, particleState);
    b.Write(15, particleState);

    // GPUCulling: compute visible objects using pre-baked input buffers.
    b.AddPass("GPUCulling");
    b.Read(16, cullingInput);
    b.Write(16, cullingOutput);

    RenderGraphCompiler c;
    c.Compile(b);
    const auto& g = c.GetCompiledGraph();

    EXPECT_TRUE(g.HasDirectDep(2, 0));
    EXPECT_TRUE(g.HasDirectDep(2, 1));
    EXPECT_TRUE(g.HasDirectDep(3, 2));
    EXPECT_TRUE(g.HasDirectDep(4, 1));
    EXPECT_TRUE(g.HasDirectDep(5, 4));
    EXPECT_TRUE(g.HasDirectDep(6, 1));
    EXPECT_TRUE(g.HasDirectDep(6, 3));
    EXPECT_TRUE(g.HasDirectDep(7, 1));
    EXPECT_TRUE(g.HasDirectDep(8, 3));
    EXPECT_TRUE(g.HasDirectDep(9, 8));
    EXPECT_TRUE(g.HasDirectDep(10, 3));
    EXPECT_TRUE(g.HasDirectDep(10, 9));
    EXPECT_TRUE(g.HasDirectDep(10, 5));
    EXPECT_TRUE(g.HasDirectDep(10, 6));
    EXPECT_TRUE(g.HasDirectDep(11, 10));
    EXPECT_TRUE(g.HasDirectDep(12, 11));
    EXPECT_TRUE(g.HasDirectDep(12, 7));
    EXPECT_TRUE(g.HasDirectDep(13, 12));

    EXPECT_LT(FindTopoPos(g, 0), FindTopoPos(g, 2));
    EXPECT_LT(FindTopoPos(g, 1), FindTopoPos(g, 2));
    EXPECT_LT(FindTopoPos(g, 2), FindTopoPos(g, 3));
    EXPECT_LT(FindTopoPos(g, 1), FindTopoPos(g, 4));
    EXPECT_LT(FindTopoPos(g, 4), FindTopoPos(g, 5));
    EXPECT_LT(FindTopoPos(g, 5), FindTopoPos(g, 10));
    EXPECT_LT(FindTopoPos(g, 6), FindTopoPos(g, 10));
    EXPECT_LT(FindTopoPos(g, 8), FindTopoPos(g, 9));
    EXPECT_LT(FindTopoPos(g, 9), FindTopoPos(g, 10));
    EXPECT_LT(FindTopoPos(g, 10), FindTopoPos(g, 11));
    EXPECT_LT(FindTopoPos(g, 11), FindTopoPos(g, 12));
    EXPECT_LT(FindTopoPos(g, 12), FindTopoPos(g, 13));

    auto bloomIt = g.lifetimes.find(bloomAV1.id);
    auto gradeIt = g.lifetimes.find(colorGradeV1.id);
    ASSERT_TRUE(bloomIt != g.lifetimes.end());
    ASSERT_TRUE(gradeIt != g.lifetimes.end());
    EXPECT_LT(bloomIt->second.end, gradeIt->second.begin);

    auto aliasIt = g.aliasMap.find(colorGradeV1.id);
    ASSERT_TRUE(aliasIt != g.aliasMap.end());
    EXPECT_EQ(aliasIt->second, bloomAV1.id);

    EXPECT_TRUE(g.directDeps[14].empty());
    EXPECT_TRUE(g.directDeps[15].empty());
    EXPECT_TRUE(g.directDeps[16].empty());

    fmt::print("RDG TopoOrder:");
    for (const auto& pass : g.passesInTopo)
        fmt::print(" {}({})", pass.node, pass.name);
    fmt::print("\n");

    (void)backbufferV3;
}