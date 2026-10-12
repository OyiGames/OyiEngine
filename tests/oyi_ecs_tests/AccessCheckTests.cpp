#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/AccessLog.h>
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
#include <oyi_job/JobSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace {

using oyi::core::SystemAllocator;
using oyi::ecs::AccessEntry;
using oyi::ecs::AccessKind;
using oyi::ecs::AccessLog;
using oyi::ecs::AccessScope;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::MessageMask;
using oyi::ecs::Phase;
using oyi::ecs::ResourceMask;
using oyi::ecs::Schedule;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::SystemId;
using oyi::ecs::World;
using oyi::job::JobSystem;
using namespace oyi::ecs_fixture;

constexpr JobSystem::Config kThreeWorkers{
    .mWorkerCount = 3, .mMaxJobs = 256, .mMaxCounters = 128, .mSpinIterations = 8};

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

[[nodiscard]] SystemDesc Describe(std::string_view name, ComponentMask read, ComponentMask write,
                                  oyi::ecs::SystemFunction function, void* data) {
    return SystemDesc{.mName           = name,
                      .mPhase          = Phase::Variable,
                      .mRead           = read,
                      .mWrite          = write,
                      .mExclude        = ComponentMask{},
                      .mOptional       = ComponentMask{},
                      .mSends          = MessageMask{},
                      .mReceives       = MessageMask{},
                      .mReadResources  = ResourceMask{},
                      .mWriteResources = ResourceMask{},
                      .mMainThread     = false,
                      .mFunction       = function,
                      .mData           = data};
}

struct Types {
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mVelocity;
    ComponentTypeIndex mHealth;
    World* mWorld = nullptr;
    EntityId mVictim;
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

void ReadHealth(SystemContext& context) {
    const auto* const types = context.GetData<Types>();
    context.ForEachChunk([&](ChunkView& view) { (void)context.Read<Health>(view, types->mHealth); });
}

/// Declares Position only, but writes Health through a World pointer smuggled in its data: the
/// static declaration cannot see it, the runtime check can.
void RogueSystem(SystemContext& context) {
    const auto* const types = context.GetData<Types>();
    context.ForEachChunk([&](ChunkView& view) { (void)context.Read<Position>(view, types->mPosition); });
    auto* const health = static_cast<Health*>(types->mWorld->GetComponentForWrite(types->mVictim, types->mHealth));
    health->mCurrent   = 1;
}

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    Types mTypes{.mPosition = *mWorld.GetComponents().Register<Position>(),
                 .mVelocity = *mWorld.GetComponents().Register<Velocity>(),
                 .mHealth   = *mWorld.GetComponents().Register<Health>(),
                 .mWorld    = &mWorld,
                 .mVictim   = EntityId::Invalid()};
    Schedule mSchedule{mAllocator, mWorld};

    Fixture() {
        for (uint32_t i = 0; i < 700; ++i) {
            const EntityId entity = *mWorld.CreateEntity(MaskOf({mTypes.mPosition, mTypes.mVelocity, mTypes.mHealth}));
            mWorld.GetForWrite<Velocity>(entity)->mX = static_cast<float>(i % 3);
            if (i == 0) {
                mTypes.mVictim = entity;
            }
        }
        mSchedule.SetAccessCheckMode(Schedule::AccessCheckMode::Record);
    }
};

} // namespace

TEST_CASE("the default access check mode follows the build", "[ecs][access]") {
    SystemAllocator allocator;
    World world(allocator);
    const Schedule schedule(allocator, world);
#if OYI_ACCESS_CHECKS
    CHECK(schedule.GetAccessCheckMode() == Schedule::AccessCheckMode::Assert);
#else
    CHECK(schedule.GetAccessCheckMode() == Schedule::AccessCheckMode::Off);
#endif
}

TEST_CASE("an access log deduplicates by chunk and type and upgrades reads to writes", "[ecs][access]") {
    Fixture f;
    const oyi::ecs::Archetype& archetype = f.mWorld.GetArchetype(0);
    AccessLog log(f.mAllocator);
    CHECK(log.IsEmpty());
    for (uint32_t chunk = 0; chunk < archetype.GetChunkCount(); ++chunk) {
        log.Record(archetype, chunk, f.mTypes.mPosition, AccessKind::Read);
        log.Record(archetype, chunk, f.mTypes.mPosition, AccessKind::Read);
        log.Record(archetype, chunk, f.mTypes.mVelocity, AccessKind::Read);
        log.Record(archetype, chunk, f.mTypes.mVelocity, AccessKind::Write);
    }
    CHECK(log.GetEntries().size() == static_cast<size_t>(archetype.GetChunkCount()) * 2);
    REQUIRE(log.Find(archetype, 0, f.mTypes.mPosition) != nullptr);
    CHECK(log.Find(archetype, 0, f.mTypes.mPosition)->mKind == AccessKind::Read);
    CHECK(log.Find(archetype, 0, f.mTypes.mVelocity)->mKind == AccessKind::Write);
    CHECK(log.Find(archetype, 0, f.mTypes.mHealth) == nullptr);

    AccessLog other(f.mAllocator);
    other.Record(archetype, 0, f.mTypes.mPosition, AccessKind::Read);
    CHECK(log.FindConflict(other) == nullptr); // read-read
    other.Record(archetype, 0, f.mTypes.mVelocity, AccessKind::Read);
    REQUIRE(log.FindConflict(other) != nullptr);
    CHECK(log.FindConflict(other)->mType == f.mTypes.mVelocity);
    log.Clear();
    CHECK(log.IsEmpty());
    CHECK(log.FindConflict(other) == nullptr);
}

TEST_CASE("the accessors record into the scope of the calling thread only", "[ecs][access]") {
    Fixture f;
    AccessLog log(f.mAllocator);
    {
        const AccessScope scope(&log);
        (void)f.mWorld.Get<Position>(f.mTypes.mVictim);
        (void)f.mWorld.GetForWrite<Health>(f.mTypes.mVictim);
        f.mWorld.ForEachChunk(MaskOf({f.mTypes.mVelocity}), ComponentMask{},
                              [&](ChunkView& view) { (void)view.Read<Velocity>(f.mTypes.mVelocity); });
    }
    (void)f.mWorld.GetForWrite<Position>(f.mTypes.mVictim); // outside: not recorded
#if OYI_ACCESS_CHECKS
    const oyi::ecs::Archetype& archetype = f.mWorld.GetArchetype(0);
    CHECK(log.GetEntries().size() == static_cast<size_t>(archetype.GetChunkCount()) + 2);
    CHECK(log.Find(archetype, 0, f.mTypes.mPosition)->mKind == AccessKind::Read);
    CHECK(log.Find(archetype, 0, f.mTypes.mHealth)->mKind == AccessKind::Write);
    CHECK(log.Find(archetype, 0, f.mTypes.mVelocity)->mKind == AccessKind::Read);
#else
    CHECK(log.IsEmpty());
#endif
    CHECK(oyi::ecs::GetCurrentAccessLog() == nullptr);
}

TEST_CASE("well-behaved systems pass the phase check and leave their tables behind", "[ecs][access][schedule]") {
    Fixture f;
    const SystemId move = *f.mSchedule.AddSystem(
        Describe("move", MaskOf({f.mTypes.mVelocity}), MaskOf({f.mTypes.mPosition}), &MoveSystem, &f.mTypes));
    const SystemId reader =
        *f.mSchedule.AddSystem(Describe("health", MaskOf({f.mTypes.mHealth}), {}, &ReadHealth, &f.mTypes));
    REQUIRE(f.mSchedule.Build().has_value());
    JobSystem jobs(f.mAllocator, kThreeWorkers);
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(f.mSchedule.GetViolations().empty());
#if OYI_ACCESS_CHECKS
    const oyi::ecs::Archetype& archetype = f.mWorld.GetArchetype(0);
    const AccessLog& moved               = f.mSchedule.GetAccessLog(move);
    CHECK(moved.GetEntries().size() == static_cast<size_t>(archetype.GetChunkCount()) * 2);
    CHECK(moved.Find(archetype, 0, f.mTypes.mPosition)->mKind == AccessKind::Write);
    CHECK(moved.Find(archetype, 0, f.mTypes.mVelocity)->mKind == AccessKind::Read);
    CHECK(f.mSchedule.GetAccessLog(reader).GetEntries().size() == archetype.GetChunkCount());
#else
    CHECK(f.mSchedule.GetAccessLog(move).IsEmpty());
    CHECK(f.mSchedule.GetAccessLog(reader).IsEmpty());
#endif
}

TEST_CASE("a system writing through a smuggled World pointer is caught, in the parallel and the serial run",
          "[ecs][access][schedule]") {
    const auto run = [](bool parallel) {
        Fixture f;
        const SystemId rogue =
            *f.mSchedule.AddSystem(Describe("rogue", MaskOf({f.mTypes.mPosition}), {}, &RogueSystem, &f.mTypes));
        const SystemId reader =
            *f.mSchedule.AddSystem(Describe("health", MaskOf({f.mTypes.mHealth}), {}, &ReadHealth, &f.mTypes));
        REQUIRE(f.mSchedule.Build().has_value()); // no declared conflict: the pair stays unordered
        if (parallel) {
            JobSystem jobs(f.mAllocator, kThreeWorkers);
            REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
        } else {
            REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
        }
        CHECK(f.mWorld.Get<Health>(f.mTypes.mVictim)->mCurrent == 1); // Record mode lets it through
        const std::span<const Schedule::AccessViolation> violations = f.mSchedule.GetViolations();
#if OYI_ACCESS_CHECKS
        REQUIRE(violations.size() == 2);
        CHECK(violations[0].mKind == Schedule::AccessViolationKind::Undeclared);
        CHECK(violations[0].mFirst == rogue);
        CHECK_FALSE(violations[0].mSecond.IsValid());
        CHECK(violations[0].mType == f.mTypes.mHealth);
        CHECK(violations[0].mAccess == AccessKind::Write);
        CHECK(violations[0].mArchetype == &f.mWorld.GetArchetype(0));
        CHECK(violations[0].mChunk == 0);
        CHECK(violations[1].mKind == Schedule::AccessViolationKind::Unordered);
        CHECK(violations[1].mFirst == rogue);
        CHECK(violations[1].mSecond == reader);
        CHECK(violations[1].mType == f.mTypes.mHealth);
        CHECK(f.mSchedule.GetDesc(violations[1].mSecond).mName == "health");
#else
        CHECK(violations.empty());
        (void)rogue;
        (void)reader;
#endif
        // The next phase starts clean.
        f.mSchedule.SetAccessCheckMode(Schedule::AccessCheckMode::Off);
        REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
        CHECK(f.mSchedule.GetViolations().empty());
        CHECK(f.mSchedule.GetAccessLog(rogue).IsEmpty());
    };
    run(true);
    run(false);
}

TEST_CASE("the state hash agrees between the serial and the parallel run and notices a change",
          "[ecs][access][schedule][job]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        (void)*f.mSchedule.AddSystem(
            Describe("move", MaskOf({f.mTypes.mVelocity}), MaskOf({f.mTypes.mPosition}), &MoveSystem, &f.mTypes));
        REQUIRE(f.mSchedule.Build().has_value());
        if (parallel) {
            JobSystem jobs(f.mAllocator, kThreeWorkers);
            for (int frame = 0; frame < 5; ++frame) {
                REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
            }
        } else {
            for (int frame = 0; frame < 5; ++frame) {
                REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
            }
        }
        const uint64_t hash = f.mWorld.ComputeStateHash();
        f.mWorld.GetForWrite<Position>(f.mTypes.mVictim)->mY += 1.0f;
        CHECK(f.mWorld.ComputeStateHash() != hash);
        return hash;
    };
    CHECK(simulate(false) == simulate(true));
    SystemAllocator allocator;
    const World empty(allocator);
    const World alsoEmpty(allocator);
    CHECK(empty.ComputeStateHash() == alsoEmpty.ComputeStateHash());
}
