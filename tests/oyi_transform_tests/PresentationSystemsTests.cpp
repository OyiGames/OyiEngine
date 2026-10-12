#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_job/JobSystem.h>
#include <oyi_transform/Components.gen.h> // IWYU pragma: keep
#include <oyi_transform/Components.h>
#include <oyi_transform/PresentationSystems.h>
#include <oyi_transform/TransformSystems.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

using Catch::Approx;
using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::Phase;
using oyi::ecs::Schedule;
using oyi::ecs::World;
using oyi::ecs::WorldKind;
using oyi::job::JobSystem;
using oyi::transform::AddPresentationSystems;
using oyi::transform::AddTransformSystems;
using oyi::transform::LocalToWorld;
using oyi::transform::PresentationContext;
using oyi::transform::PresentationSystemIds;
using oyi::transform::RegisterTransformComponents;
using oyi::transform::RenderTransform;
using oyi::transform::TransformHierarchy;
using oyi::transform::TransformSystemIds;
using oyi::transform::TransformTypes;
using oyi::transform::Translation;
using oyi::transform::WorldBounds;

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld;
    TransformTypes mTypes{*RegisterTransformComponents(mWorld)};
    TransformHierarchy mHierarchy{mAllocator, mTypes};
    PresentationContext mContext{.mTypes = mTypes};
    Schedule mSchedule{mAllocator, mWorld};
    TransformSystemIds mTransform{};
    PresentationSystemIds mPresentation{};

    explicit Fixture(WorldKind kind = WorldKind::Client)
        : mWorld(mAllocator, World::Config{.mChunkSize = 16 * 1024, .mKind = kind}) {
        mTransform    = *AddTransformSystems(mSchedule, mHierarchy);
        mPresentation = *AddPresentationSystems(mSchedule, mContext, mTransform);
        REQUIRE(mSchedule.Build().has_value());
    }

    [[nodiscard]] EntityId SpawnRendered(float x) {
        const EntityId entity = *mWorld.CreateEntity(
            MaskOf({mTypes.mTranslation, mTypes.mLocalToWorld, mTypes.mRenderTransform, mTypes.mWorldBounds}));
        mWorld.GetForWrite<Translation>(entity)->mValue.mX = x;
        return entity;
    }

    void Frame(float alpha) {
        mWorld.GetEnvironmentForWrite().mInterpolationAlpha = alpha;
        REQUIRE(mSchedule.RunFrameSerial().has_value());
    }
};

} // namespace

TEST_CASE("the render transform blends the previous tick and the current one by alpha", "[transform][presentation]") {
    Fixture f;
    const EntityId entity = f.SpawnRendered(10.0f);
    // Frame 1: the previous sample is the identity, the current tick puts x at 10.
    f.Frame(0.5f);
    CHECK(f.mWorld.Get<LocalToWorld>(entity)->mM[9] == 10.0f);
    CHECK(f.mWorld.Get<RenderTransform>(entity)->mPrevious[9] == 0.0f);
    CHECK(f.mWorld.Get<RenderTransform>(entity)->mM[9] == Approx(5.0f));
    // Frame 2: the simulation moves to 30; the sample now holds 10.
    f.mWorld.GetForWrite<Translation>(entity)->mValue.mX = 30.0f;
    f.Frame(0.25f);
    CHECK(f.mWorld.Get<RenderTransform>(entity)->mPrevious[9] == 10.0f);
    CHECK(f.mWorld.Get<RenderTransform>(entity)->mM[9] == Approx(15.0f));
    f.Frame(1.0f);
    CHECK(f.mWorld.Get<RenderTransform>(entity)->mM[9] == Approx(30.0f));
    // The basis stays the identity throughout.
    CHECK(f.mWorld.Get<RenderTransform>(entity)->mM[0] == 1.0f);
    CHECK(f.mWorld.Get<RenderTransform>(entity)->mM[4] == 1.0f);
    // Alpha outside [0, 1] is clamped.
    f.Frame(3.0f);
    CHECK(f.mWorld.Get<RenderTransform>(entity)->mM[9] == Approx(30.0f));
}

TEST_CASE("every chunk aggregates the bounds of its translations, in the parallel run too",
          "[transform][presentation][job]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        std::vector<EntityId> entities;
        entities.reserve(2000);
        for (uint32_t i = 0; i < 2000; ++i) {
            entities.push_back(f.SpawnRendered(static_cast<float>(i) - 1000.0f));
        }
        f.mWorld.GetForWrite<Translation>(entities[7])->mValue.mY = -3.0f;
        f.mWorld.GetEnvironmentForWrite().mInterpolationAlpha     = 1.0f;
        if (parallel) {
            JobSystem jobs(
                f.mAllocator,
                JobSystem::Config{.mWorkerCount = 3, .mMaxJobs = 1024, .mMaxCounters = 256, .mSpinIterations = 8});
            REQUIRE(f.mSchedule.RunFrame(jobs).has_value());
        } else {
            REQUIRE(f.mSchedule.RunFrameSerial().has_value());
        }
        uint32_t chunks = 0;
        f.mWorld.ForEachChunk(
            MaskOf({f.mTypes.mLocalToWorld, f.mTypes.mWorldBounds}), ComponentMask{}, [&](oyi::ecs::ChunkView& view) {
                const auto* const bounds =
                    static_cast<const WorldBounds*>(view.GetChunkComponent(f.mTypes.mWorldBounds));
                const std::span<const LocalToWorld> world = view.Read<LocalToWorld>(f.mTypes.mLocalToWorld);
                REQUIRE(bounds != nullptr);
                CHECK(bounds->mCount == world.size());
                float minX = world[0].mM[9];
                float maxX = world[0].mM[9];
                float minY = world[0].mM[10];
                for (const LocalToWorld& matrix : world) {
                    minX = std::min(minX, matrix.mM[9]);
                    maxX = std::max(maxX, matrix.mM[9]);
                    minY = std::min(minY, matrix.mM[10]);
                }
                CHECK(bounds->mMin.mX == minX);
                CHECK(bounds->mMax.mX == maxX);
                CHECK(bounds->mMin.mY == minY);
                ++chunks;
            });
        CHECK(chunks >= 4);
        return f.mWorld.ComputeStateHash();
    };
    CHECK(simulate(false) == simulate(true));
}

TEST_CASE("a server world keeps the bounds but drops the render systems", "[transform][presentation][worldkind]") {
    Fixture server(WorldKind::Server);
    CHECK_FALSE(server.mSchedule.IsSystemIncluded(server.mPresentation.mSample));
    CHECK_FALSE(server.mSchedule.IsSystemIncluded(server.mPresentation.mInterpolate));
    CHECK(server.mSchedule.IsSystemIncluded(server.mPresentation.mBounds));
    const EntityId entity = server.SpawnRendered(4.0f);
    server.Frame(0.5f);
    CHECK(server.mWorld.Get<LocalToWorld>(entity)->mM[9] == 4.0f);
    CHECK(server.mWorld.Get<RenderTransform>(entity)->mM[9] == 0.0f); // never touched
    const auto* const bounds =
        static_cast<const WorldBounds*>(server.mWorld.GetChunkComponent(entity, server.mTypes.mWorldBounds));
    REQUIRE(bounds != nullptr);
    CHECK(bounds->mMax.mX == 4.0f);
}
