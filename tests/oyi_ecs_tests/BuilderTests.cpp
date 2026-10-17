#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/Builders.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentOf;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::MessageMask;
using oyi::ecs::MessageOf;
using oyi::ecs::MessageTypeIndex;
using oyi::ecs::Phase;
using oyi::ecs::RegisterModule;
using oyi::ecs::Schedule;
using oyi::ecs::System;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::World;
using namespace oyi::ecs_fixture;

struct Types {
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mVelocity;
    MessageTypeIndex mNudge;
};

void MoveSystem(SystemContext& context) {
    const auto* const types = context.GetData<Types>();
    context.ForEachChunk([&](ChunkView& view) {
        const std::span<const Velocity> velocities = context.Read<Velocity>(view, types->mVelocity);
        const std::span<Position> positions        = context.Write<Position>(view, types->mPosition);
        for (size_t i = 0; i < positions.size(); ++i) {
            positions[i].mX += velocities[i].mX;
        }
    });
}

void NudgeSystem(SystemContext& context) {
    const auto* const types = context.GetData<Types>();
    const auto received     = context.Received<Nudge>(types->mNudge);
    for (size_t i = 0; i < received.mTargets.size(); ++i) {
        if (auto* const velocity = context.LookupForWrite<Velocity>(types->mVelocity, received.mTargets[i]);
            velocity != nullptr) {
            velocity->mX += received.mPayloads[i].mX;
        }
    }
}

} // namespace

TEST_CASE("a generated module registers its components and messages in one call", "[ecs][builders][module]") {
    SystemAllocator allocator;
    World world(allocator);
    CHECK(kModule.mName == "oyi::ecs_fixture");
    CHECK(kModule.mSchemaHash == kSchemaHash);
    CHECK(kModule.mComponents.size() == 9);
    CHECK(kModule.mMessages.size() == 3);
    CHECK(kModule.mStructs.size() == 1); // BrainState: a state block, not a message
    CHECK_FALSE(ComponentOf<Position>(world).IsValid());

    REQUIRE(RegisterModule(world, kModule).has_value());
    CHECK(world.GetComponents().GetCount() == 9);
    CHECK(world.GetMessages().GetCount() == 3);
    CHECK(ComponentOf<Position>(world).IsValid());
    CHECK(ComponentOf<BrainRef>(world).IsValid());
    CHECK(MessageOf<Nudge>(world).IsValid());
    CHECK_FALSE(MessageOf<BrainState>(world).IsValid());
    CHECK(ComponentOf<Position>(world) == world.GetComponents().Find<Position>());

    REQUIRE(RegisterModule(world, kModule).has_value()); // idempotent
    CHECK(world.GetComponents().GetCount() == 9);
    CHECK(world.GetMessages().GetCount() == 3);
}

TEST_CASE("the system builder writes the same description as the hand-written one", "[ecs][builders]") {
    SystemAllocator allocator;
    World world(allocator);
    REQUIRE(RegisterModule(world, kModule).has_value());
    Types types{.mPosition = ComponentOf<Position>(world),
                .mVelocity = ComponentOf<Velocity>(world),
                .mNudge    = MessageOf<Nudge>(world)};
    const ComponentTypeIndex frozen = ComponentOf<Frozen>(world);

    const auto built = System("move", world)
                           .Phase(Phase::FixedSimulation)
                           .Reads<Velocity>()
                           .Writes<Position>()
                           .Excludes<Frozen>()
                           .Optional<Velocity>()
                           .Sends<Ping>()
                           .Receives<Nudge, Damage>()
                           .ReadsEnvironment()
                           .MainThread()
                           .Fn(&MoveSystem, &types);
    REQUIRE(built.has_value());
    CHECK(built->mName == "move");
    CHECK(built->mPhase == Phase::FixedSimulation);
    CHECK(built->mRead == ComponentMask{}.With(types.mVelocity));
    CHECK(built->mWrite == ComponentMask{}.With(types.mPosition));
    CHECK(built->mExclude == ComponentMask{}.With(frozen));
    CHECK(built->mOptional == ComponentMask{}.With(types.mVelocity));
    CHECK(built->mSends == MessageMask{}.With(MessageOf<Ping>(world)));
    CHECK(built->mReceives == MessageMask{}.With(types.mNudge).With(MessageOf<Damage>(world)));
    CHECK(built->mReadResources.Test(oyi::ecs::kEnvironmentResource));
    CHECK(built->mReadResources.Test(world.GetResources().Find("oyi::ecs::Environment")));
    CHECK(built->mWriteResources == oyi::ecs::ResourceMask{});
    CHECK(built->mMainThread);
    CHECK(built->mFunction == &MoveSystem);
    CHECK(built->mData == &types);

    // Defaults: Variable phase, nothing declared, worker thread.
    const auto bare = System("bare", world).Fn(&MoveSystem);
    REQUIRE(bare.has_value());
    CHECK(bare->mPhase == Phase::Variable);
    CHECK(bare->mRead == ComponentMask{});
    CHECK_FALSE(bare->mMainThread);
    CHECK(bare->mData == nullptr);

    // Refusals: no function, no name, a type the world does not know.
    CHECK(System("move", world).Fn(nullptr).error().mCode == ErrorCode::InvalidArgument);
    CHECK(System("", world).Fn(&MoveSystem).error().mCode == ErrorCode::InvalidArgument);
    World bareWorld(allocator);
    REQUIRE(bareWorld.GetComponents().Register<Position>().has_value());
    auto missing = System("move", bareWorld).Writes<Position>().Reads<Velocity, Health>();
    CHECK(missing.Fn(&MoveSystem).error().mCode == ErrorCode::NotFound);
    CHECK(missing.GetMissingType() == "oyi::ecs_fixture::Velocity");
    CHECK(System("move", bareWorld).Receives<Nudge>().Build().error().mCode == ErrorCode::NotFound);
    CHECK(System("move", bareWorld).Writes<Position>().Build().has_value()); // Build needs no function
}

TEST_CASE("systems built this way schedule and run like any other", "[ecs][builders][schedule]") {
    SystemAllocator allocator;
    World world(allocator);
    REQUIRE(RegisterModule(world, kModule).has_value());
    Types types{.mPosition = ComponentOf<Position>(world),
                .mVelocity = ComponentOf<Velocity>(world),
                .mNudge    = MessageOf<Nudge>(world)};
    Schedule schedule(allocator, world);
    const auto nudge =
        schedule.AddSystem(*System("nudge", world).Writes<Velocity>().Receives<Nudge>().Fn(&NudgeSystem, &types));
    const auto move =
        schedule.AddSystem(*System("move", world).Reads<Velocity>().Writes<Position>().Fn(&MoveSystem, &types));
    REQUIRE(nudge.has_value());
    REQUIRE(move.has_value());
    REQUIRE(schedule.AddOrder(*nudge, *move).has_value());
    REQUIRE(schedule.Build().has_value());

    const EntityId entity = *world.CreateEntity(ComponentMask{}.With(types.mPosition).With(types.mVelocity));
    world.GetForWrite<Velocity>(entity)->mX = 1.0f;
    REQUIRE(schedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(world.Get<Position>(entity)->mX == 1.0f);
}
