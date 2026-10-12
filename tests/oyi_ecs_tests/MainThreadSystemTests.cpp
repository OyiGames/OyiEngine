#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Message.h>
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
#include <thread>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::MessageMask;
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

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

[[nodiscard]] SystemDesc Describe(std::string_view name, Phase phase, ComponentMask read, ComponentMask write,
                                  bool mainThread, oyi::ecs::SystemFunction function, void* data) {
    return SystemDesc{.mName           = name,
                      .mPhase          = phase,
                      .mRead           = read,
                      .mWrite          = write,
                      .mExclude        = ComponentMask{},
                      .mOptional       = ComponentMask{},
                      .mSends          = MessageMask{},
                      .mReceives       = MessageMask{},
                      .mReadResources  = oyi::ecs::ResourceMask{},
                      .mWriteResources = oyi::ecs::ResourceMask{},
                      .mMainThread     = mainThread,
                      .mFunction       = function,
                      .mData           = data};
}

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    Schedule mSchedule{mAllocator, mWorld};
};

struct ThreadProbe {
    std::thread::id mMain;
    std::atomic<bool> mRanOnMain = false;
    std::atomic<bool> mRan       = false;

    static void Run(SystemContext& context) {
        auto* const self = context.GetData<ThreadProbe>();
        self->mRanOnMain.store(std::this_thread::get_id() == self->mMain);
        self->mRan.store(true);
    }
};

struct Trace {
    std::vector<std::string_view> mOrder; ///< only safe because every pair is ordered
};

void TraceA(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("A");
}
void TraceM(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("M");
}
void TraceB(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("B");
}

/// A job that only finishes once the main-thread system has run: it cannot pass unless the two
/// overlap.
struct Handshake {
    std::atomic<bool> mMainDone  = false;
    std::atomic<bool> mJobPassed = false;

    static void MainSide(SystemContext& context) { context.GetData<Handshake>()->mMainDone.store(true); }

    static void JobSide(SystemContext& context) {
        auto* const self = context.GetData<Handshake>();
        while (!self->mMainDone.load()) {
            std::this_thread::yield();
        }
        self->mJobPassed.store(true);
    }
};

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
        }
    });
}

/// Main-thread system: random access over the entities it owns, the managed-layer pattern.
void DrainSystem(SystemContext& context) {
    const auto* const types = context.GetData<Types>();
    context.ForEachChunk([&](ChunkView& view) {
        for (const EntityId entity : view.GetEntities()) {
            auto* const health = context.LookupForWrite<Health>(types->mHealth, entity);
            health->mCurrent   = static_cast<uint16_t>(health->mCurrent > 0 ? health->mCurrent - 1 : 0);
        }
    });
}

} // namespace

TEST_CASE("a main-thread system runs on the thread that called RunPhase", "[ecs][schedule][mainthread]") {
    Fixture f;
    ThreadProbe probe;
    probe.mMain = std::this_thread::get_id();
    (void)*f.mSchedule.AddSystem(Describe("probe", Phase::Begin, {}, {}, true, &ThreadProbe::Run, &probe));
    REQUIRE(f.mSchedule.Build().has_value());

    JobSystem jobs(f.mAllocator, kThreeWorkers);
    for (int round = 0; round < 10; ++round) {
        probe.mRan.store(false);
        REQUIRE(f.mSchedule.RunPhase(Phase::Begin, jobs).has_value());
        CHECK(probe.mRan.load());
        CHECK(probe.mRanOnMain.load());
    }
    CHECK(jobs.GetStats().mInFlightJobs == 0);
}

TEST_CASE("order edges hold in both directions between jobs and main-thread systems", "[ecs][schedule][mainthread]") {
    Fixture f;
    Trace trace;
    const SystemId a = *f.mSchedule.AddSystem(Describe("a", Phase::Begin, {}, {}, false, &TraceA, &trace));
    const SystemId m = *f.mSchedule.AddSystem(Describe("m", Phase::Begin, {}, {}, true, &TraceM, &trace));
    const SystemId b = *f.mSchedule.AddSystem(Describe("b", Phase::Begin, {}, {}, false, &TraceB, &trace));
    REQUIRE(f.mSchedule.AddOrder(a, m).has_value());
    REQUIRE(f.mSchedule.AddOrder(m, b).has_value());
    REQUIRE(f.mSchedule.Build().has_value());

    JobSystem jobs(f.mAllocator, kThreeWorkers);
    for (int round = 0; round < 30; ++round) {
        trace.mOrder.clear();
        REQUIRE(f.mSchedule.RunPhase(Phase::Begin, jobs).has_value());
        CHECK(trace.mOrder == std::vector<std::string_view>{"A", "M", "B"});
    }
}

TEST_CASE("jobs are kicked before the main thread runs its systems", "[ecs][schedule][mainthread]") {
    Fixture f;
    Handshake handshake;
    (void)*f.mSchedule.AddSystem(Describe("job", Phase::Begin, {}, {}, false, &Handshake::JobSide, &handshake));
    (void)*f.mSchedule.AddSystem(Describe("main", Phase::Begin, {}, {}, true, &Handshake::MainSide, &handshake));
    REQUIRE(f.mSchedule.Build().has_value());

    JobSystem jobs(f.mAllocator, kThreeWorkers);
    for (int round = 0; round < 10; ++round) {
        handshake.mMainDone.store(false);
        handshake.mJobPassed.store(false);
        REQUIRE(f.mSchedule.RunPhase(Phase::Begin, jobs).has_value());
        CHECK(handshake.mJobPassed.load());
    }
}

TEST_CASE("main-thread systems obey the conflict rules and match the serial run", "[ecs][schedule][mainthread]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        Types types{.mPosition = f.mPosition, .mVelocity = f.mVelocity, .mHealth = f.mHealth};
        for (uint32_t i = 0; i < 500; ++i) {
            const EntityId entity = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mVelocity, f.mHealth}));
            f.mWorld.GetForWrite<Velocity>(entity)->mX     = static_cast<float>(i % 5);
            f.mWorld.GetForWrite<Health>(entity)->mCurrent = static_cast<uint16_t>(i % 7);
        }
        const SystemId move  = *f.mSchedule.AddSystem(Describe("move", Phase::FixedSimulation, MaskOf({f.mVelocity}),
                                                               MaskOf({f.mPosition}), false, &MoveSystem, &types));
        const SystemId drain = *f.mSchedule.AddSystem(
            Describe("drain", Phase::FixedSimulation, {}, MaskOf({f.mHealth}), true, &DrainSystem, &types));
        // A second writer of Health on a worker: refused until it is ordered against the main one.
        const SystemId drainAgain = *f.mSchedule.AddSystem(
            Describe("drain-again", Phase::FixedSimulation, {}, MaskOf({f.mHealth}), false, &DrainSystem, &types));
        (void)move;
        REQUIRE_FALSE(f.mSchedule.Build().has_value());
        REQUIRE(f.mSchedule.AddOrder(drain, drainAgain).has_value());
        REQUIRE(f.mSchedule.Build().has_value());

        if (parallel) {
            JobSystem jobs(f.mAllocator, kThreeWorkers);
            for (int frame = 0; frame < 4; ++frame) {
                REQUIRE(f.mSchedule.RunPhase(Phase::FixedSimulation, jobs).has_value());
            }
        } else {
            for (int frame = 0; frame < 4; ++frame) {
                REQUIRE(f.mSchedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
            }
        }
        std::vector<float> state;
        f.mWorld.ForEachChunk(MaskOf({f.mPosition, f.mHealth}), ComponentMask{}, [&](ChunkView& view) {
            const std::span<const Position> positions = view.Read<Position>(f.mPosition);
            const std::span<const Health> healths     = view.Read<Health>(f.mHealth);
            for (size_t i = 0; i < positions.size(); ++i) {
                state.push_back(positions[i].mX);
                state.push_back(static_cast<float>(healths[i].mCurrent));
            }
        });
        return state;
    };
    const auto serial   = simulate(false);
    const auto parallel = simulate(true);
    CHECK(serial.size() == 1000);
    CHECK(serial == parallel);
}
