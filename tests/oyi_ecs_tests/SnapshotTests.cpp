#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::Phase;
using oyi::ecs::Schedule;
using oyi::ecs::Snapshot;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::World;
using namespace oyi::ecs_fixture;

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
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    Snapshot mSnapshot{mAllocator};
};

} // namespace

TEST_CASE("a snapshot copies the requested types of every entity that has any of them", "[ecs][snapshot]") {
    Fixture f;
    const EntityId mover                      = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mVelocity}));
    const EntityId still                      = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mHealth}));
    const EntityId hidden                     = *f.mWorld.CreateEntity(MaskOf({f.mHealth}));
    f.mWorld.GetForWrite<Position>(mover)->mX = 1.0f;
    f.mWorld.GetForWrite<Velocity>(mover)->mZ = 2.0f;
    f.mWorld.GetForWrite<Position>(still)->mX = 3.0f;
    f.mWorld.AdvanceVersion();

    f.mSnapshot.Capture(f.mWorld, MaskOf({f.mPosition, f.mVelocity}));
    CHECK(f.mSnapshot.GetVersion() == f.mWorld.GetVersion());
    CHECK(f.mSnapshot.GetEntityCount() == 2);
    CHECK(f.mSnapshot.Contains(mover));
    CHECK(f.mSnapshot.Contains(still));
    CHECK_FALSE(f.mSnapshot.Contains(hidden)); // Health was not requested
    CHECK(f.mSnapshot.Get<Position>(mover, f.mPosition)->mX == 1.0f);
    CHECK(f.mSnapshot.Get<Velocity>(mover, f.mVelocity)->mZ == 2.0f);
    CHECK(f.mSnapshot.Get<Position>(still, f.mPosition)->mX == 3.0f);
    CHECK(f.mSnapshot.Get<Velocity>(still, f.mVelocity) == nullptr); // captured entity, absent type
    CHECK(f.mSnapshot.Get<Health>(still, f.mHealth) == nullptr);     // type not in the snapshot
    CHECK(f.mSnapshot.GetColumn(f.mPosition).size() == 2 * sizeof(Position));

    // The copy is independent of the live data.
    f.mWorld.GetForWrite<Position>(mover)->mX = 100.0f;
    CHECK(f.mSnapshot.Get<Position>(mover, f.mPosition)->mX == 1.0f);
}

TEST_CASE("a recycled entity index does not resolve against the old row", "[ecs][snapshot]") {
    Fixture f;
    const EntityId first = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    f.mSnapshot.Capture(f.mWorld, MaskOf({f.mPosition}));
    REQUIRE(f.mSnapshot.Contains(first));

    REQUIRE(f.mWorld.DestroyEntity(first).has_value());
    const EntityId reused = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    REQUIRE(reused.mIndex == first.mIndex);
    CHECK(f.mSnapshot.Contains(first));        // the snapshot still describes the old entity
    CHECK_FALSE(f.mSnapshot.Contains(reused)); // and not the one that took its index
    CHECK(f.mSnapshot.Get<Position>(reused, f.mPosition) == nullptr);

    f.mSnapshot.Clear();
    CHECK(f.mSnapshot.GetEntityCount() == 0);
    CHECK_FALSE(f.mSnapshot.Contains(first));
}

TEST_CASE("captures reuse memory and reflect the world at each sync point", "[ecs][snapshot]") {
    Fixture f;
    std::vector<EntityId> entities;
    entities.reserve(500);
    for (uint32_t i = 0; i < 500; ++i) {
        entities.push_back(*f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mHealth})));
        f.mWorld.GetForWrite<Health>(entities.back())->mCurrent = static_cast<uint16_t>(i);
    }
    f.mSnapshot.Capture(f.mWorld, MaskOf({f.mHealth}));
    for (uint32_t i = 0; i < 500; ++i) {
        REQUIRE(f.mSnapshot.Get<Health>(entities[i], f.mHealth)->mCurrent == i);
    }
    // A second capture of the same world allocates nothing new.
    const auto liveBefore = f.mAllocator.GetStats().mLiveAllocations;
    f.mSnapshot.Capture(f.mWorld, MaskOf({f.mHealth}));
    CHECK(f.mAllocator.GetStats().mLiveAllocations == liveBefore);

    REQUIRE(f.mWorld.DestroyEntity(entities[7]).has_value());
    f.mWorld.GetForWrite<Health>(entities[8])->mCurrent = 999;
    f.mSnapshot.Capture(f.mWorld, MaskOf({f.mHealth}));
    CHECK(f.mSnapshot.GetEntityCount() == 499);
    CHECK_FALSE(f.mSnapshot.Contains(entities[7]));
    CHECK(f.mSnapshot.Get<Health>(entities[8], f.mHealth)->mCurrent == 999);
}

TEST_CASE("the schedule captures a snapshot at the end of the chosen phase", "[ecs][snapshot][schedule]") {
    Fixture f;
    Schedule schedule(f.mAllocator, f.mWorld);
    const EntityId entity = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    struct Bump {
        ComponentTypeIndex mPosition;
        static void Run(SystemContext& context) {
            const auto* const self = context.GetData<Bump>();
            context.ForEachChunk([&](ChunkView& view) {
                for (Position& position : context.Write<Position>(view, self->mPosition)) {
                    position.mX += 1.0f;
                }
            });
        }
    };
    Bump bump{.mPosition = f.mPosition};
    REQUIRE(schedule
                .AddSystem(SystemDesc{.mName           = "bump",
                                      .mPhase          = Phase::FixedSimulation,
                                      .mRead           = ComponentMask{},
                                      .mWrite          = MaskOf({f.mPosition}),
                                      .mExclude        = ComponentMask{},
                                      .mOptional       = ComponentMask{},
                                      .mSends          = oyi::ecs::MessageMask{},
                                      .mReceives       = oyi::ecs::MessageMask{},
                                      .mReadResources  = oyi::ecs::ResourceMask{},
                                      .mWriteResources = oyi::ecs::ResourceMask{},
                                      .mMainThread     = false,
                                      .mFunction       = &Bump::Run,
                                      .mData           = &bump})
                .has_value());
    REQUIRE(schedule.Build().has_value());
    schedule.SetSnapshotCapture(&f.mSnapshot, MaskOf({f.mPosition}), Phase::FixedSimulation);

    REQUIRE(schedule.RunPhaseSerial(Phase::Input).has_value());
    CHECK(f.mSnapshot.GetEntityCount() == 0); // not the capture phase
    REQUIRE(schedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
    CHECK(f.mSnapshot.GetEntityCount() == 1);
    CHECK(f.mSnapshot.Get<Position>(entity, f.mPosition)->mX == 1.0f);
    CHECK(f.mSnapshot.GetVersion() == f.mWorld.GetVersion());

    REQUIRE(schedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
    CHECK(f.mSnapshot.Get<Position>(entity, f.mPosition)->mX == 2.0f);
    schedule.SetSnapshotCapture(nullptr, ComponentMask{}, Phase::FixedSimulation);
    REQUIRE(schedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
    CHECK(f.mSnapshot.Get<Position>(entity, f.mPosition)->mX == 2.0f); // capture disabled
}
