#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_job/JobSystem.h>
#include <oyi_transform/Affine.h>
#include <oyi_transform/Components.gen.h> // IWYU pragma: keep
#include <oyi_transform/Components.h>
#include <oyi_transform/TransformSystems.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <numbers>
#include <utility>
#include <vector>

namespace {

using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::Phase;
using oyi::ecs::Schedule;
using oyi::ecs::World;
using oyi::job::JobSystem;
using oyi::transform::AddTransformSystems;
using oyi::transform::AffineConstRef;
using oyi::transform::Float3;
using oyi::transform::LocalToWorld;
using oyi::transform::ManualTransform;
using oyi::transform::Parent;
using oyi::transform::PreviousParent;
using oyi::transform::RegisterTransformComponents;
using oyi::transform::Rotation;
using oyi::transform::Scale;
using oyi::transform::TransformHierarchy;
using oyi::transform::TransformPoint;
using oyi::transform::TransformTypes;
using oyi::transform::Translation;

constexpr float kEpsilon = 1e-4f;

[[nodiscard]] bool Near(Float3 a, Float3 b) {
    return std::fabs(a.mX - b.mX) < kEpsilon && std::fabs(a.mY - b.mY) < kEpsilon && std::fabs(a.mZ - b.mZ) < kEpsilon;
}

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    TransformTypes mTypes{*RegisterTransformComponents(mWorld)};
    TransformHierarchy mHierarchy{mAllocator, mTypes};
    Schedule mSchedule{mAllocator, mWorld};

    Fixture() {
        REQUIRE(AddTransformSystems(mSchedule, mHierarchy).has_value());
        REQUIRE(mSchedule.Build().has_value());
    }

    [[nodiscard]] EntityId Root(Float3 translation) {
        const EntityId entity = *mWorld.CreateEntity(MaskOf({mTypes.mTranslation, mTypes.mLocalToWorld}));
        mWorld.GetForWrite<Translation>(entity)->mValue = translation;
        return entity;
    }

    [[nodiscard]] EntityId Child(EntityId parent, Float3 translation) {
        const EntityId entity = *mWorld.CreateEntity(
            MaskOf({mTypes.mTranslation, mTypes.mLocalToParent, mTypes.mLocalToWorld, mTypes.mParent}));
        mWorld.GetForWrite<Translation>(entity)->mValue = translation;
        mWorld.GetForWrite<Parent>(entity)->mValue      = parent;
        return entity;
    }

    [[nodiscard]] Float3 Origin(EntityId entity) const {
        return TransformPoint(AffineConstRef{mWorld.Get<LocalToWorld>(entity)->mM}, Float3{});
    }

    void Run() { REQUIRE(mSchedule.RunPhaseSerial(Phase::TransformPropagate).has_value()); }
};

} // namespace

TEST_CASE("TRS composes into LocalToWorld for roots, with optional rotation and scale", "[transform][systems]") {
    Fixture f;
    const EntityId plain = f.Root(Float3{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f});
    const EntityId full  = *f.mWorld.CreateEntity(
        MaskOf({f.mTypes.mTranslation, f.mTypes.mRotation, f.mTypes.mScale, f.mTypes.mLocalToWorld}));
    f.mWorld.GetForWrite<Translation>(full)->mValue = Float3{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f};
    const float half                                = std::numbers::pi_v<float> / 4.0f;
    *f.mWorld.GetForWrite<Rotation>(full) =
        Rotation{.mX = 0.0f, .mY = std::sin(half), .mZ = 0.0f, .mW = std::cos(half)};
    f.mWorld.GetForWrite<Scale>(full)->mValue = Float3{.mX = 2.0f, .mY = 2.0f, .mZ = 2.0f};

    f.Run();
    CHECK(Near(f.Origin(plain), Float3{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f}));
    CHECK(Near(TransformPoint(AffineConstRef{f.mWorld.Get<LocalToWorld>(full)->mM},
                              Float3{.mX = 1.0f, .mY = 0.0f, .mZ = 0.0f}),
               Float3{.mX = 1.0f, .mY = 2.0f, .mZ = 1.0f}));
    CHECK(f.mHierarchy.GetRebuildCount() == 0); // no parented entity, nothing to rebuild
}

TEST_CASE("a hierarchy propagates in depth order and rebuilds only when the topology changes", "[transform][systems]") {
    Fixture f;
    const EntityId root       = f.Root(Float3{.mX = 10.0f, .mY = 0.0f, .mZ = 0.0f});
    const EntityId child      = f.Child(root, Float3{.mX = 1.0f, .mY = 0.0f, .mZ = 0.0f});
    const EntityId grandchild = f.Child(child, Float3{.mX = 0.0f, .mY = 1.0f, .mZ = 0.0f});

    f.Run();
    CHECK(f.mHierarchy.GetRebuildCount() == 1);
    CHECK(f.mHierarchy.GetOrder().size() == 2);
    CHECK(f.mHierarchy.GetOrder()[0] == child);
    CHECK(f.mHierarchy.GetOrder()[1] == grandchild);
    CHECK(Near(f.Origin(child), Float3{.mX = 11.0f, .mY = 0.0f, .mZ = 0.0f}));
    CHECK(Near(f.Origin(grandchild), Float3{.mX = 11.0f, .mY = 1.0f, .mZ = 0.0f}));
    // The system added its own state at the sync point.
    CHECK(f.mWorld.HasComponent(child, f.mTypes.mPreviousParent));
    CHECK(f.mWorld.Get<PreviousParent>(grandchild)->mValue == child);

    f.Run();
    CHECK(f.mHierarchy.GetRebuildCount() == 1); // quiet frame

    // Reparent the grandchild to the root (a sync-point write) and move the root.
    f.mWorld.GetForWrite<Parent>(grandchild)->mValue = root;
    f.mWorld.GetForWrite<Translation>(root)->mValue  = Float3{.mX = 20.0f, .mY = 0.0f, .mZ = 0.0f};
    f.Run();
    CHECK(f.mHierarchy.GetRebuildCount() == 2);
    CHECK(Near(f.Origin(child), Float3{.mX = 21.0f, .mY = 0.0f, .mZ = 0.0f}));
    CHECK(Near(f.Origin(grandchild), Float3{.mX = 20.0f, .mY = 1.0f, .mZ = 0.0f}));
    CHECK(f.mWorld.Get<PreviousParent>(grandchild)->mValue == root);
}

TEST_CASE("ManualTransform keeps the built-in systems away", "[transform][systems]") {
    Fixture f;
    const EntityId manual =
        *f.mWorld.CreateEntity(MaskOf({f.mTypes.mTranslation, f.mTypes.mLocalToWorld, f.mTypes.mManualTransform}));
    f.mWorld.GetForWrite<Translation>(manual)->mValue = Float3{.mX = 5.0f, .mY = 5.0f, .mZ = 5.0f};
    f.mWorld.GetForWrite<LocalToWorld>(manual)->mM[9] = 42.0f;
    f.Run();
    CHECK(f.mWorld.Get<LocalToWorld>(manual)->mM[9] == 42.0f);
}

TEST_CASE("an entity that loses its parent becomes a root and drops its system state", "[transform][systems]") {
    Fixture f;
    const EntityId root  = f.Root(Float3{.mX = 10.0f, .mY = 0.0f, .mZ = 0.0f});
    const EntityId child = f.Child(root, Float3{.mX = 1.0f, .mY = 0.0f, .mZ = 0.0f});
    f.Run();
    CHECK(Near(f.Origin(child), Float3{.mX = 11.0f, .mY = 0.0f, .mZ = 0.0f}));

    REQUIRE(f.mWorld.RemoveComponent(child, f.mTypes.mParent).has_value());
    f.Run(); // composes as a root; the orphan sweep asks for PreviousParent to go
    CHECK(Near(f.Origin(child), Float3{.mX = 1.0f, .mY = 0.0f, .mZ = 0.0f}));
    CHECK_FALSE(f.mWorld.HasComponent(child, f.mTypes.mPreviousParent));
    CHECK(f.mHierarchy.GetOrder().empty());
}

TEST_CASE("a child of a destroyed parent keeps its last matrix", "[transform][systems]") {
    Fixture f;
    const EntityId root  = f.Root(Float3{.mX = 10.0f, .mY = 0.0f, .mZ = 0.0f});
    const EntityId child = f.Child(root, Float3{.mX = 1.0f, .mY = 0.0f, .mZ = 0.0f});
    f.Run();
    REQUIRE(f.mWorld.DestroyEntity(root).has_value());
    f.Run();
    CHECK(Near(f.Origin(child), Float3{.mX = 11.0f, .mY = 0.0f, .mZ = 0.0f}));
    CHECK(f.mHierarchy.GetOrder().size() == 1);
}

TEST_CASE("the parallel schedule computes the same matrices as the serial one", "[transform][systems][job]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        std::vector<EntityId> all;
        for (uint32_t i = 0; i < 300; ++i) {
            const auto fi       = static_cast<float>(i);
            const EntityId root = f.Root(Float3{.mX = fi, .mY = 0.0f, .mZ = -fi});
            const EntityId a    = f.Child(root, Float3{.mX = 1.0f, .mY = fi * 0.5f, .mZ = 0.0f});
            const EntityId b    = f.Child(root, Float3{.mX = -1.0f, .mY = 0.0f, .mZ = 2.0f});
            const EntityId c    = f.Child(a, Float3{.mX = 0.0f, .mY = 1.0f, .mZ = 0.0f});
            all.insert(all.end(), {root, a, b, c});
        }
        if (parallel) {
            JobSystem jobs(
                f.mAllocator,
                JobSystem::Config{.mWorkerCount = 3, .mMaxJobs = 256, .mMaxCounters = 64, .mSpinIterations = 8});
            for (int frame = 0; frame < 3; ++frame) {
                REQUIRE(f.mSchedule.RunPhase(Phase::TransformPropagate, jobs).has_value());
            }
        } else {
            for (int frame = 0; frame < 3; ++frame) {
                f.Run();
            }
        }
        std::vector<float> origins;
        for (const EntityId entity : all) {
            const Float3 origin = f.Origin(entity);
            origins.insert(origins.end(), {origin.mX, origin.mY, origin.mZ});
        }
        return std::pair{f.mHierarchy.GetRebuildCount(), origins};
    };
    const auto serial   = simulate(false);
    const auto parallel = simulate(true);
    CHECK(serial.first == 1);
    CHECK(parallel.first == 1);
    CHECK(serial.second == parallel.second);
    CHECK(serial.second.size() == size_t{300} * 4 * 3);
}
