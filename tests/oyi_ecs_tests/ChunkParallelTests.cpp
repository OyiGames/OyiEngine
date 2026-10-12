#include <oyi_core/Expected.h>
#include <oyi_core/Memory.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/AccessLog.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Query.h>
#include <oyi_ecs/Resource.h>
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

using oyi::core::MemoryTag;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::CommandBuffer;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::MessageBuffer;
using oyi::ecs::MessageInbox;
using oyi::ecs::MessageMask;
using oyi::ecs::MessageTypeIndex;
using oyi::ecs::Phase;
using oyi::ecs::Query;
using oyi::ecs::ResourceMask;
using oyi::ecs::Schedule;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::SystemId;
using oyi::ecs::World;
using oyi::job::JobSystem;
using namespace oyi::ecs_fixture;

constexpr JobSystem::Config kThreeWorkers{
    .mWorkerCount = 3, .mMaxJobs = 1024, .mMaxCounters = 256, .mSpinIterations = 8};
constexpr uint32_t kEntityCount = 6000; ///< a dozen chunks or so

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

[[nodiscard]] SystemDesc Describe(std::string_view name, Phase phase, ComponentMask read, ComponentMask write,
                                  MessageMask sends, MessageMask receives, bool mainThread,
                                  oyi::ecs::SystemFunction function, void* data) {
    return SystemDesc{.mName           = name,
                      .mPhase          = phase,
                      .mRead           = read,
                      .mWrite          = write,
                      .mExclude        = ComponentMask{},
                      .mOptional       = ComponentMask{},
                      .mSends          = sends,
                      .mReceives       = receives,
                      .mReadResources  = ResourceMask{},
                      .mWriteResources = ResourceMask{},
                      .mMainThread     = mainThread,
                      .mFunction       = function,
                      .mData           = data};
}

struct Received {
    std::vector<EntityId> mTargets;
    std::vector<float> mValues;
};

struct Types {
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mVelocity;
    ComponentTypeIndex mHealth;
    MessageTypeIndex mNudge;
    EntityId mFarAway; ///< an entity in the last chunk, for the cross-chunk test
    Received* mReceived = nullptr;
    std::thread::id mMain{std::this_thread::get_id()};
    std::atomic<uint32_t> mOffMain = 0;
    std::atomic<uint32_t> mTasks   = 0;
    bool mMisbehave                = false;
};

/// Position += Velocity for the chunk; the first entity of the chunk gets its chunk number as
/// Health and as a Nudge, both deferred through the task's own buffers.
void MoveTask(SystemContext& task, ChunkView& view) {
    auto* const types = task.GetData<Types>();
    types->mTasks.fetch_add(1);
    if (std::this_thread::get_id() != types->mMain) {
        types->mOffMain.fetch_add(1);
    }
    const std::span<const Velocity> velocities = task.Read<Velocity>(view, types->mVelocity);
    const std::span<Position> positions        = task.Write<Position>(view, types->mPosition);
    for (size_t i = 0; i < positions.size(); ++i) {
        positions[i].mX += velocities[i].mX;
    }
    const EntityId first  = view.GetEntities()[0];
    const auto chunkIndex = static_cast<uint16_t>(view.GetChunkIndex() + 1);
    task.GetCommands().Set(first, types->mHealth, Health{.mCurrent = chunkIndex, .mMax = 100, .mLastAttacker = {}});
    task.Send(types->mNudge, first, Nudge{.mX = static_cast<float>(chunkIndex)});
    // Reading another chunk's velocity is fine (nobody writes it); writing its position is not.
    (void)task.Lookup<Velocity>(types->mVelocity, types->mFarAway);
    if (types->mMisbehave && view.GetChunkIndex() == 0 && task.IsChunkTask()) {
        task.LookupForWrite<Position>(types->mPosition, types->mFarAway)->mY = 1.0f;
    }
}

void MoveSystem(SystemContext& context) {
    const oyi::core::Result dispatched = context.ParallelForEachChunk(&MoveTask);
    CHECK(dispatched.has_value());
}

/// Copies what arrived, in delivery order.
void ReceiveSystem(SystemContext& context) {
    const auto* const types = context.GetData<Types>();
    const auto nudges       = context.Received<Nudge>(types->mNudge);
    for (size_t i = 0; i < nudges.mTargets.size(); ++i) {
        types->mReceived->mTargets.push_back(nudges.mTargets[i]);
        types->mReceived->mValues.push_back(nudges.mPayloads[i].mX);
    }
}

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    Received mReceived;
    Types mTypes{.mPosition = *mWorld.GetComponents().Register<Position>(),
                 .mVelocity = *mWorld.GetComponents().Register<Velocity>(),
                 .mHealth   = *mWorld.GetComponents().Register<Health>(),
                 .mNudge    = *mWorld.GetMessages().Register<Nudge>(),
                 .mFarAway  = EntityId::Invalid(),
                 .mReceived = &mReceived};
    Schedule mSchedule{mAllocator, mWorld};
    uint32_t mChunkCount = 0;

    explicit Fixture(bool mainThread = false) {
        for (uint32_t i = 0; i < kEntityCount; ++i) {
            const EntityId entity = *mWorld.CreateEntity(MaskOf({mTypes.mPosition, mTypes.mVelocity, mTypes.mHealth}));
            mWorld.GetForWrite<Velocity>(entity)->mX = static_cast<float>(i % 7);
            mTypes.mFarAway                          = entity;
        }
        mChunkCount = mWorld.GetArchetype(0).GetChunkCount();
        REQUIRE(mChunkCount >= 4);
        (void)*mSchedule.AddSystem(Describe("move", Phase::Variable, MaskOf({mTypes.mVelocity}),
                                            MaskOf({mTypes.mPosition}), MessageMask{}.With(mTypes.mNudge), {},
                                            mainThread, &MoveSystem, &mTypes));
        (void)*mSchedule.AddSystem(Describe("receive", Phase::End, {}, {}, {}, MessageMask{}.With(mTypes.mNudge), false,
                                            &ReceiveSystem, &mTypes));
        REQUIRE(mSchedule.Build().has_value());
        mSchedule.SetAccessCheckMode(Schedule::AccessCheckMode::Record);
    }

    [[nodiscard]] oyi::core::Result RunFrame(JobSystem* jobs, uint64_t frame) {
        if (jobs == nullptr) {
            const auto variable = mSchedule.RunPhaseSerial(Phase::Variable);
            return variable.has_value() ? mSchedule.RunPhaseSerial(Phase::End) : variable;
        }
        jobs->BeginFrame(MemoryTag::ForFrame(frame));
        auto result = mSchedule.RunPhase(Phase::Variable, *jobs);
        if (result.has_value()) {
            result = mSchedule.RunPhase(Phase::End, *jobs);
        }
        jobs->FreeFrame(MemoryTag::ForFrame(frame));
        return result;
    }

    void CheckChunkResults(uint32_t frames) {
        uint32_t chunk = 0;
        mWorld.ForEachChunk(MaskOf({mTypes.mPosition}), ComponentMask{}, [&](ChunkView& view) {
            const std::span<const Position> positions  = view.Read<Position>(mTypes.mPosition);
            const std::span<const Velocity> velocities = view.Read<Velocity>(mTypes.mVelocity);
            const std::span<const Health> healths      = view.Read<Health>(mTypes.mHealth);
            for (size_t i = 0; i < positions.size(); ++i) {
                CHECK(positions[i].mX == velocities[i].mX * static_cast<float>(frames));
            }
            CHECK(healths[0].mCurrent == chunk + 1);
            ++chunk;
        });
        CHECK(chunk == mChunkCount);
    }
};

} // namespace

TEST_CASE("chunk tasks run side by side and their deferred work lands in chunk order",
          "[ecs][chunkpar][schedule][job]") {
    Fixture f;
    JobSystem jobs(f.mAllocator, kThreeWorkers);
    REQUIRE(f.RunFrame(&jobs, 1).has_value());
    CHECK(f.mTypes.mTasks.load() == f.mChunkCount);
    // The main thread helps while it waits and may take every task before the workers wake, so
    // where the tasks ran is not asserted; the serial test pins the no-worker case.
    CHECK(f.mSchedule.GetViolations().empty());
    f.CheckChunkResults(1);
    CHECK(f.mSchedule.GetLastReplayStats().mApplied == 0); // End phase replayed nothing
    REQUIRE(f.mReceived.mTargets.size() == f.mChunkCount);
    for (uint32_t chunk = 0; chunk < f.mChunkCount; ++chunk) {
        CHECK(f.mReceived.mValues[chunk] == static_cast<float>(chunk + 1));
        CHECK(f.mReceived.mTargets[chunk] == f.mWorld.GetArchetype(0).GetEntities(chunk)[0]);
    }
    CHECK(jobs.GetStats().mInFlightJobs == 0);
    CHECK(jobs.GetFrameHeapStats().mUsedBlocks == 0);
    // Frames reuse the task buffers.
    REQUIRE(f.RunFrame(&jobs, 2).has_value());
    f.CheckChunkResults(2);
    CHECK(f.mReceived.mTargets.size() == static_cast<size_t>(f.mChunkCount) * 2);
}

TEST_CASE("the serial run takes the same tasks and ends in the same state", "[ecs][chunkpar][schedule][job]") {
    Fixture serial;
    REQUIRE(serial.RunFrame(nullptr, 1).has_value());
    CHECK(serial.mTypes.mOffMain.load() == 0);
    CHECK(serial.mTypes.mTasks.load() == serial.mChunkCount);
    CHECK(serial.mSchedule.GetViolations().empty());
    serial.CheckChunkResults(1);

    Fixture parallel;
    JobSystem jobs(parallel.mAllocator, kThreeWorkers);
    REQUIRE(parallel.RunFrame(&jobs, 1).has_value());
    CHECK(serial.mWorld.ComputeStateHash() == parallel.mWorld.ComputeStateHash());
    CHECK(serial.mReceived.mTargets == parallel.mReceived.mTargets);
    CHECK(serial.mReceived.mValues == parallel.mReceived.mValues);
}

TEST_CASE("a chunk task writing another chunk is reported as a cross-chunk violation", "[ecs][chunkpar][access]") {
    const auto run = [](bool parallel) {
        Fixture f;
        f.mTypes.mMisbehave = true;
        if (parallel) {
            JobSystem jobs(f.mAllocator, kThreeWorkers);
            REQUIRE(f.RunFrame(&jobs, 1).has_value());
        } else {
            REQUIRE(f.RunFrame(nullptr, 1).has_value());
        }
        // Record mode lets the write through; the violation itself is checked below on a phase
        // that ran last (violations are per phase).
        return f.mWorld.Get<Position>(f.mTypes.mFarAway)->mY;
    };
    CHECK(run(true) == 1.0f);
    CHECK(run(false) == 1.0f);

    Fixture f;
    f.mTypes.mMisbehave = true;
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    const std::span<const Schedule::AccessViolation> violations = f.mSchedule.GetViolations();
#if OYI_ACCESS_CHECKS
    REQUIRE(violations.size() == 1);
    CHECK(violations[0].mKind == Schedule::AccessViolationKind::CrossChunk);
    CHECK(violations[0].mType == f.mTypes.mPosition);
    CHECK(violations[0].mAccess == oyi::ecs::AccessKind::Write);
    CHECK(violations[0].mChunk == f.mChunkCount - 1);
    CHECK(f.mSchedule.GetDesc(violations[0].mFirst).mName == "move");
    // The merged table of the system covers every chunk it touched.
    CHECK(f.mSchedule.GetAccessLog(violations[0].mFirst).GetEntries().size() >= static_cast<size_t>(f.mChunkCount) * 2);
#else
    CHECK(violations.empty());
#endif
}

TEST_CASE("a main-thread system fans out chunk tasks too", "[ecs][chunkpar][schedule][mainthread]") {
    Fixture f(true);
    JobSystem jobs(f.mAllocator, kThreeWorkers);
    REQUIRE(f.RunFrame(&jobs, 1).has_value());
    CHECK(f.mTypes.mTasks.load() == f.mChunkCount);
    f.CheckChunkResults(1);
    CHECK(f.mReceived.mTargets.size() == f.mChunkCount);
    CHECK(jobs.GetStats().mInFlightJobs == 0);
}

TEST_CASE("a context without a schedule runs the chunk tasks inline", "[ecs][chunkpar]") {
    Fixture f;
    Query query(f.mAllocator, MaskOf({f.mTypes.mPosition, f.mTypes.mVelocity}), ComponentMask{});
    CommandBuffer commands(f.mAllocator);
    MessageBuffer outbox(f.mAllocator);
    MessageInbox inbox(f.mAllocator);
    const SystemDesc desc = f.mSchedule.GetDesc(SystemId{.mValue = 0});
    SystemContext context(f.mWorld, query, commands, outbox, inbox, desc, 1, 0, f.mAllocator);
    CHECK_FALSE(context.IsChunkTask());
    REQUIRE(context.ParallelForEachChunk(&MoveTask).has_value());
    CHECK(f.mTypes.mTasks.load() == f.mChunkCount);
    CHECK(f.mTypes.mOffMain.load() == 0);
    CHECK(commands.GetCommandCount() == f.mChunkCount); // one shared buffer in this mode
}
