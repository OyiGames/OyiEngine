#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_job/JobSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::Phase;
using oyi::ecs::Schedule;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::SystemId;
using oyi::ecs::World;
using oyi::job::JobSystem;
using namespace oyi::ecs_fixture;

constexpr JobSystem::Config kThreeWorkers{
    .mWorkerCount = 3, .mMaxJobs = 256, .mMaxCounters = 128, .mSpinIterations = 8};

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    ComponentTypeIndex mFrozen   = *mWorld.GetComponents().Register<Frozen>();
    Schedule mSchedule{mAllocator, mWorld};
};

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

/// Shared by the system functions below: which type indices to use.
struct Types {
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mVelocity;
    ComponentTypeIndex mHealth;
};

void MoveSystem(SystemContext& context) {
    const auto* const types = context.GetData<Types>();
    context.ForEachChunk([&](ChunkView& view) {
        const std::span<const Velocity> velocities = context.Read<Velocity>(view, types->mVelocity);
        const std::span<Position> positions        = context.Write<Position>(view, types->mPosition);
        for (size_t i = 0; i < positions.size(); ++i) {
            positions[i].mX += velocities[i].mX;
            positions[i].mY += velocities[i].mY;
        }
    });
}

/// Loses one health per run and asks the sync point to destroy whoever reaches zero.
void DamageSystem(SystemContext& context) {
    const auto* const types = context.GetData<Types>();
    context.ForEachChunk([&](ChunkView& view) {
        const std::span<Health> healths          = context.Write<Health>(view, types->mHealth);
        const std::span<const EntityId> entities = view.GetEntities();
        for (size_t i = 0; i < healths.size(); ++i) {
            if (healths[i].mCurrent > 0) {
                --healths[i].mCurrent;
            }
            if (healths[i].mCurrent == 0) {
                context.GetCommands().DestroyEntity(entities[i]);
            }
        }
    });
}

struct Tally {
    Types mTypes;
    std::atomic<uint32_t> mRows          = 0;
    std::atomic<uint32_t> mChangedChunks = 0;
    std::atomic<uint32_t> mRuns          = 0;
};

/// Reads positions; counts rows and the chunks whose positions changed since its previous run.
void TallySystem(SystemContext& context) {
    auto* const tally = context.GetData<Tally>();
    tally->mRuns.fetch_add(1);
    context.ForEachChunk([&](ChunkView& view) {
        tally->mRows.fetch_add(static_cast<uint32_t>(context.Read<Position>(view, tally->mTypes.mPosition).size()));
        if (view.HasChangedSince(tally->mTypes.mPosition, context.GetLastVersion())) {
            tally->mChangedChunks.fetch_add(1);
        }
    });
}

struct Trace {
    std::vector<std::string_view> mOrder; ///< appended without locks: only a correct order makes that safe
};

void TraceA(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("A");
}
void TraceB(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("B");
}
void TraceC(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("C");
}
void TraceD(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("D");
}

[[nodiscard]] SystemDesc Describe(std::string_view name, Phase phase, ComponentMask read, ComponentMask write,
                                  oyi::ecs::SystemFunction function, void* data) {
    return SystemDesc{.mName     = name,
                      .mPhase    = phase,
                      .mRead     = read,
                      .mWrite    = write,
                      .mExclude  = ComponentMask{},
                      .mOptional = ComponentMask{},
                      .mSends    = oyi::ecs::MessageMask{},
                      .mReceives = oyi::ecs::MessageMask{},
                      .mFunction = function,
                      .mData     = data};
}

void Populate(Fixture& f, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const EntityId entity = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mVelocity, f.mHealth}));
        f.mWorld.GetForWrite<Velocity>(entity)->mX     = static_cast<float>(i % 7);
        f.mWorld.GetForWrite<Velocity>(entity)->mY     = 1.0f;
        f.mWorld.GetForWrite<Health>(entity)->mCurrent = static_cast<uint16_t>(1 + (i % 9));
    }
}

} // namespace

TEST_CASE("Build rejects unordered conflicts, cycles and cross-phase edges", "[ecs][schedule]") {
    Fixture f;
    Types types{.mPosition = f.mPosition, .mVelocity = f.mVelocity, .mHealth = f.mHealth};
    const ComponentMask position = MaskOf({f.mPosition});
    const ComponentMask velocity = MaskOf({f.mVelocity});

    const SystemId move =
        *f.mSchedule.AddSystem(Describe("move", Phase::FixedSimulation, velocity, position, &MoveSystem, &types));
    const SystemId teleport = *f.mSchedule.AddSystem(
        Describe("teleport", Phase::FixedSimulation, ComponentMask{}, position, &MoveSystem, &types));
    const SystemId tallyLater =
        *f.mSchedule.AddSystem(Describe("tally", Phase::Presentation, position, ComponentMask{}, &TallySystem, &types));
    (void)tallyLater;

    // Two writers of Position in one phase without an order: refused, and the pair is reported.
    const auto unordered = f.mSchedule.Build();
    REQUIRE_FALSE(unordered.has_value());
    CHECK(unordered.error().mCode == ErrorCode::InvalidState);
    CHECK(f.mSchedule.GetLastConflict().mFirst == move);
    CHECK(f.mSchedule.GetLastConflict().mSecond == teleport);
    CHECK_FALSE(f.mSchedule.IsBuilt());

    // Edges only live inside a phase.
    CHECK(f.mSchedule.AddOrder(move, tallyLater).error().mCode == ErrorCode::InvalidArgument);
    CHECK(f.mSchedule.AddOrder(move, move).error().mCode == ErrorCode::InvalidArgument);

    REQUIRE(f.mSchedule.AddOrder(move, teleport).has_value());
    // A reader of Position in the same phase also needs an order.
    const SystemId reader = *f.mSchedule.AddSystem(
        Describe("reader", Phase::FixedSimulation, position, ComponentMask{}, &TallySystem, &types));
    REQUIRE_FALSE(f.mSchedule.Build().has_value());
    REQUIRE(f.mSchedule.AddOrder(teleport, reader).has_value());
    REQUIRE(f.mSchedule.Build().has_value());
    CHECK(f.mSchedule.IsBuilt());
    CHECK(f.mSchedule.GetOrder(Phase::FixedSimulation).size() == 3);
    CHECK(f.mSchedule.GetOrder(Phase::FixedSimulation)[0] == move);
    CHECK(f.mSchedule.GetOrder(Phase::FixedSimulation)[2] == reader);
    CHECK(f.mSchedule.GetOrder(Phase::Presentation).size() == 1);
    CHECK(f.mSchedule.AddSystem(Describe("late", Phase::End, {}, {}, &TraceA, nullptr)).error().mCode ==
          ErrorCode::InvalidState);
}

TEST_CASE("a cycle of order edges is rejected", "[ecs][schedule]") {
    Fixture f;
    Trace trace;
    const SystemId a = *f.mSchedule.AddSystem(Describe("a", Phase::Begin, {}, {}, &TraceA, &trace));
    const SystemId b = *f.mSchedule.AddSystem(Describe("b", Phase::Begin, {}, {}, &TraceB, &trace));
    REQUIRE(f.mSchedule.AddOrder(a, b).has_value());
    REQUIRE(f.mSchedule.AddOrder(b, a).has_value());
    const auto built = f.mSchedule.Build();
    REQUIRE_FALSE(built.has_value());
    CHECK(built.error().mCode == ErrorCode::InvalidState);
    CHECK(f.mSchedule.RunPhaseSerial(Phase::Begin).error().mCode == ErrorCode::InvalidState);
}

TEST_CASE("the serial run moves entities, applies commands at the phase end and skips other phases",
          "[ecs][schedule]") {
    Fixture f;
    Types types{.mPosition = f.mPosition, .mVelocity = f.mVelocity, .mHealth = f.mHealth};
    Populate(f, 10);
    const SystemId move = *f.mSchedule.AddSystem(
        Describe("move", Phase::FixedSimulation, MaskOf({f.mVelocity}), MaskOf({f.mPosition}), &MoveSystem, &types));
    const SystemId damage = *f.mSchedule.AddSystem(
        Describe("damage", Phase::FixedSimulation, {}, MaskOf({f.mHealth}), &DamageSystem, &types));
    (void)move;
    (void)damage;
    Tally tally{.mTypes = types};
    (void)*f.mSchedule.AddSystem(
        Describe("tally", Phase::Presentation, MaskOf({f.mPosition}), {}, &TallySystem, &tally));
    REQUIRE(f.mSchedule.Build().has_value());

    const uint32_t versionBefore = f.mWorld.GetVersion();
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
    CHECK(tally.mRuns.load() == 0); // Presentation did not run
    CHECK(f.mWorld.GetVersion() > versionBefore + 2);
    // Entities 0 and 9 had health 1 and were destroyed at the sync point.
    CHECK(f.mWorld.GetEntityCount() == 8);
    CHECK(f.mSchedule.GetLastReplayStats().mApplied == 2);
    uint32_t moved = 0;
    f.mWorld.ForEachChunk(MaskOf({f.mPosition}), ComponentMask{}, [&](ChunkView& view) {
        for (const Position& position : view.Read<Position>(f.mPosition)) {
            CHECK(position.mY == 1.0f);
            ++moved;
        }
    });
    CHECK(moved == 8);

    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Presentation).has_value());
    CHECK(tally.mRuns.load() == 1);
    CHECK(tally.mRows.load() == 8);
}

TEST_CASE("the change filter sees writes from the previous run but not quiet frames", "[ecs][schedule]") {
    Fixture f;
    Types types{.mPosition = f.mPosition, .mVelocity = f.mVelocity, .mHealth = f.mHealth};
    Populate(f, 5);
    Tally tally{.mTypes = types};
    const SystemId move = *f.mSchedule.AddSystem(
        Describe("move", Phase::FixedSimulation, MaskOf({f.mVelocity}), MaskOf({f.mPosition}), &MoveSystem, &types));
    const SystemId watch =
        *f.mSchedule.AddSystem(Describe("watch", Phase::Variable, MaskOf({f.mPosition}), {}, &TallySystem, &tally));
    (void)move;
    (void)watch;
    REQUIRE(f.mSchedule.Build().has_value());

    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(tally.mChangedChunks.load() == 1); // first run: everything counts as changed
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(tally.mChangedChunks.load() == 1); // nothing wrote positions in between
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(tally.mChangedChunks.load() == 2); // move wrote them
}

TEST_CASE("order edges are honoured on the job system", "[ecs][schedule][job]") {
    Fixture f;
    Trace trace;
    const SystemId a = *f.mSchedule.AddSystem(Describe("a", Phase::Begin, {}, {}, &TraceA, &trace));
    const SystemId b = *f.mSchedule.AddSystem(Describe("b", Phase::Begin, {}, {}, &TraceB, &trace));
    const SystemId c = *f.mSchedule.AddSystem(Describe("c", Phase::Begin, {}, {}, &TraceC, &trace));
    const SystemId d = *f.mSchedule.AddSystem(Describe("d", Phase::Begin, {}, {}, &TraceD, &trace));
    // a -> b -> c, b -> d, c -> d: d joins two predecessors; every pair is ordered so the
    // unsynchronised trace vector is safe.
    REQUIRE(f.mSchedule.AddOrder(a, b).has_value());
    REQUIRE(f.mSchedule.AddOrder(b, c).has_value());
    REQUIRE(f.mSchedule.AddOrder(b, d).has_value());
    REQUIRE(f.mSchedule.AddOrder(c, d).has_value());
    REQUIRE(f.mSchedule.Build().has_value());
    CHECK(f.mSchedule.GetPredecessors(d).size() == 2);

    JobSystem jobs(f.mAllocator, kThreeWorkers);
    for (int round = 0; round < 30; ++round) {
        trace.mOrder.clear();
        REQUIRE(f.mSchedule.RunPhase(Phase::Begin, jobs).has_value());
        REQUIRE(trace.mOrder.size() == 4);
        CHECK(trace.mOrder == std::vector<std::string_view>{"A", "B", "C", "D"});
    }
    CHECK(jobs.GetStats().mInFlightJobs == 0);
}

TEST_CASE("a parallel frame produces the same world as a serial frame", "[ecs][schedule][job]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        Types types{.mPosition = f.mPosition, .mVelocity = f.mVelocity, .mHealth = f.mHealth};
        Populate(f, 2000);
        Tally tally{.mTypes = types};
        const SystemId move   = *f.mSchedule.AddSystem(Describe("move", Phase::FixedSimulation, MaskOf({f.mVelocity}),
                                                                MaskOf({f.mPosition}), &MoveSystem, &types));
        const SystemId damage = *f.mSchedule.AddSystem(
            Describe("damage", Phase::FixedSimulation, {}, MaskOf({f.mHealth}), &DamageSystem, &types));
        (void)damage; // touches Health only: runs beside move, no order needed
        const SystemId watch = *f.mSchedule.AddSystem(
            Describe("watch", Phase::FixedSimulation, MaskOf({f.mPosition}), {}, &TallySystem, &tally));
        REQUIRE(f.mSchedule.AddOrder(move, watch).has_value());
        REQUIRE(f.mSchedule.Build().has_value());

        std::vector<float> positions;
        if (parallel) {
            JobSystem jobs(f.mAllocator, kThreeWorkers);
            for (int frame = 0; frame < 6; ++frame) {
                REQUIRE(f.mSchedule.RunFrame(jobs).has_value());
            }
        } else {
            for (int frame = 0; frame < 6; ++frame) {
                REQUIRE(f.mSchedule.RunFrameSerial().has_value());
            }
        }
        // After six frames every entity with health <= 6 is gone; the rest moved six times.
        std::vector<EntityId> alive;
        f.mWorld.ForEachChunk(MaskOf({f.mPosition}), ComponentMask{}, [&](ChunkView& view) {
            for (const Position& position : view.Read<Position>(f.mPosition)) {
                positions.push_back(position.mX);
                positions.push_back(position.mY);
            }
            for (const EntityId entity : view.GetEntities()) {
                alive.push_back(entity);
            }
        });
        return std::pair{alive, positions};
    };
    const auto serial   = simulate(false);
    const auto parallel = simulate(true);
    // Health 7, 8 or 9 survives six frames: three of every nine entities, 222 full cycles.
    CHECK(serial.first.size() == 666);
    CHECK(serial.first == parallel.first);
    CHECK(serial.second == parallel.second);
}

TEST_CASE("the context reports what a system may touch", "[ecs][schedule]") {
    Fixture f;
    struct Probe {
        bool mCanReadVelocity  = false;
        bool mCanWriteVelocity = false;
        bool mCanWritePosition = false;
        bool mCanReadHealth    = false;
        Types mTypes;
    };
    Probe probe{.mTypes = Types{.mPosition = f.mPosition, .mVelocity = f.mVelocity, .mHealth = f.mHealth}};
    const auto probeSystem = [](SystemContext& context) {
        auto* const self        = context.GetData<Probe>();
        self->mCanReadVelocity  = context.CanRead(self->mTypes.mVelocity);
        self->mCanWriteVelocity = context.CanWrite(self->mTypes.mVelocity);
        self->mCanWritePosition = context.CanWrite(self->mTypes.mPosition);
        self->mCanReadHealth    = context.CanRead(self->mTypes.mHealth);
    };
    (void)*f.mSchedule.AddSystem(
        Describe("probe", Phase::Input, MaskOf({f.mVelocity}), MaskOf({f.mPosition}), probeSystem, &probe));
    REQUIRE(f.mSchedule.Build().has_value());
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Input).has_value());
    CHECK(probe.mCanReadVelocity);
    CHECK_FALSE(probe.mCanWriteVelocity);
    CHECK(probe.mCanWritePosition);
    CHECK_FALSE(probe.mCanReadHealth);
}
