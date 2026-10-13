#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
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

#include <cstdint>
#include <span>
#include <string_view>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
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

struct Counters {
    ComponentTypeIndex mPosition;
    uint32_t mWrites       = 0;
    uint32_t mReads        = 0;
    uint32_t mChangedRuns  = 0; ///< reads that saw Position changed since their last run
    uint32_t mMessagesSeen = 0;
    oyi::ecs::MessageTypeIndex mPing;
};

void Writer(SystemContext& context) {
    auto* const counters = context.GetData<Counters>();
    ++counters->mWrites;
    context.ForEachChunk([&](ChunkView& view) {
        for (Position& position : context.Write<Position>(view, counters->mPosition)) {
            position.mX += 1.0f;
        }
    });
    context.Send(counters->mPing, EntityId::Invalid(), Ping{.mSerial = counters->mWrites});
}

void Reader(SystemContext& context) {
    auto* const counters = context.GetData<Counters>();
    ++counters->mReads;
    bool changed = false;
    context.ForEachChunk([&](ChunkView& view) {
        changed = changed || view.HasChangedSince(counters->mPosition, context.GetLastVersion());
        (void)context.Read<Position>(view, counters->mPosition);
    });
    counters->mChangedRuns += changed ? 1 : 0;
    counters->mMessagesSeen += static_cast<uint32_t>(context.Received<Ping>(counters->mPing).mPayloads.size());
}

[[nodiscard]] SystemDesc Describe(std::string_view name, Phase phase, ComponentMask read, ComponentMask write,
                                  MessageMask sends, MessageMask receives, oyi::ecs::SystemFunction function,
                                  void* data) {
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
                      .mMainThread     = false,
                      .mFunction       = function,
                      .mData           = data};
}

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    Counters mCounters{.mPosition = *mWorld.GetComponents().Register<Position>(),
                       .mPing     = *mWorld.GetMessages().Register<Ping>()};
    Schedule mSchedule{mAllocator, mWorld};
    SystemId mWriter;
    SystemId mReader;

    Fixture() {
        (void)*mWorld.CreateEntity(ComponentMask{}.With(mCounters.mPosition));
        mWriter =
            *mSchedule.AddSystem(Describe("writer", Phase::Variable, {}, ComponentMask{}.With(mCounters.mPosition),
                                          MessageMask{}.With(mCounters.mPing), {}, &Writer, &mCounters));
        mReader =
            *mSchedule.AddSystem(Describe("reader", Phase::Presentation, ComponentMask{}.With(mCounters.mPosition), {},
                                          {}, MessageMask{}.With(mCounters.mPing), &Reader, &mCounters));
    }
};

} // namespace

TEST_CASE("a presentation system with a rate divisor runs every Nth frame and still sees every change",
          "[ecs][downclock][schedule]") {
    Fixture f;
    REQUIRE(f.mSchedule.SetRateDivisor(f.mReader, 3).has_value());
    CHECK(f.mSchedule.GetRateDivisor(f.mReader) == 3);
    CHECK(f.mSchedule.GetRateDivisor(f.mWriter) == 1);
    REQUIRE(f.mSchedule.Build().has_value());
    JobSystem jobs(f.mAllocator,
                   JobSystem::Config{.mWorkerCount = 2, .mMaxJobs = 256, .mMaxCounters = 128, .mSpinIterations = 8});
    for (int frame = 0; frame < 9; ++frame) {
        REQUIRE(f.mSchedule.RunFrame(jobs).has_value());
    }
    CHECK(f.mCounters.mWrites == 9);
    CHECK(f.mCounters.mReads == 3);        // frames 0, 3, 6
    CHECK(f.mCounters.mChangedRuns == 3);  // the change filter spans the skipped frames
    CHECK(f.mCounters.mMessagesSeen == 7); // and so does the inbox: frames 7 and 8 are still queued
    // The serial run downclocks the same way; frame 9 is its turn and drains what waited.
    for (int frame = 0; frame < 3; ++frame) {
        REQUIRE(f.mSchedule.RunFrameSerial().has_value());
    }
    CHECK(f.mCounters.mReads == 4);
    CHECK(f.mCounters.mMessagesSeen == 10);
}

TEST_CASE("systems that affect the simulation are never downclocked", "[ecs][downclock][schedule]") {
    Fixture f;
    CHECK(f.mSchedule.SetRateDivisor(f.mWriter, 2).error().mCode == ErrorCode::InvalidArgument);
    CHECK(f.mSchedule.SetRateDivisor(f.mReader, 0).error().mCode == ErrorCode::InvalidArgument);
    REQUIRE(f.mSchedule.SetRateDivisor(f.mReader, 1).has_value());
    REQUIRE(f.mSchedule.Build().has_value());
    CHECK(f.mSchedule.SetRateDivisor(f.mReader, 2).has_value()); // tunable after Build (device profiles)
    for (int frame = 0; frame < 4; ++frame) {
        REQUIRE(f.mSchedule.RunFrameSerial().has_value());
    }
    CHECK(f.mCounters.mReads == 2);
}
