#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Facade.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/Snapshot.h>
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
#include <thread>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::Facade;
using oyi::ecs::FirewallWindow;
using oyi::ecs::MessageMask;
using oyi::ecs::MessageTypeIndex;
using oyi::ecs::PendingEntity;
using oyi::ecs::Phase;
using oyi::ecs::Schedule;
using oyi::ecs::Snapshot;
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

[[nodiscard]] MessageMask MessagesOf(std::initializer_list<MessageTypeIndex> types) {
    MessageMask mask;
    for (const MessageTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

[[nodiscard]] SystemDesc Describe(std::string_view name, Phase phase, ComponentMask read, ComponentMask write,
                                  MessageMask receives, oyi::ecs::SystemFunction function, void* data) {
    return SystemDesc{.mName           = name,
                      .mPhase          = phase,
                      .mRead           = read,
                      .mWrite          = write,
                      .mExclude        = ComponentMask{},
                      .mOptional       = ComponentMask{},
                      .mSends          = MessageMask{},
                      .mReceives       = receives,
                      .mReadResources  = oyi::ecs::ResourceMask{},
                      .mWriteResources = oyi::ecs::ResourceMask{},
                      .mMainThread     = false,
                      .mFunction       = function,
                      .mData           = data};
}

struct Types {
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mVelocity;
    ComponentTypeIndex mHealth;
    MessageTypeIndex mNudge;
};

/// Position += Velocity.
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

/// Velocity += Nudge for every targeted entity.
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

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    Types mTypes{.mPosition = *mWorld.GetComponents().Register<Position>(),
                 .mVelocity = *mWorld.GetComponents().Register<Velocity>(),
                 .mHealth   = *mWorld.GetComponents().Register<Health>(),
                 .mNudge    = *mWorld.GetMessages().Register<Nudge>()};
    Snapshot mPrev{mAllocator};
    Schedule mSchedule{mAllocator, mWorld};

    /// nudge (receives Nudge, writes Velocity) before move (reads Velocity, writes Position).
    void AddMovement() {
        const SystemId nudge = *mSchedule.AddSystem(Describe("nudge", Phase::Variable, {}, MaskOf({mTypes.mVelocity}),
                                                             MessagesOf({mTypes.mNudge}), &NudgeSystem, &mTypes));
        const SystemId move  = *mSchedule.AddSystem(Describe("move", Phase::Variable, MaskOf({mTypes.mVelocity}),
                                                             MaskOf({mTypes.mPosition}), {}, &MoveSystem, &mTypes));
        REQUIRE(mSchedule.AddOrder(nudge, move).has_value());
    }

    [[nodiscard]] EntityId Spawn(float velocity) {
        const EntityId entity = *mWorld.CreateEntity(MaskOf({mTypes.mPosition, mTypes.mVelocity, mTypes.mHealth}));
        mWorld.GetForWrite<Velocity>(entity)->mX = velocity;
        return entity;
    }
};

/// What the window observed, for the assertions outside it.
struct Observation {
    Fixture* mFixture = nullptr;
    JobSystem* mJobs  = nullptr;
    EntityId mEntity;
    uint32_t mRuns         = 0;
    bool mOnMain           = false;
    uint32_t mInFlight     = 0xFFFF'FFFFu;
    float mLiveX           = -1.0f;
    float mPrevX           = -1.0f;
    uint32_t mVersion      = 0;
    uint32_t mPendingAtEnd = 0;
    std::thread::id mMain{std::this_thread::get_id()};
};

void Observe(FirewallWindow& window) {
    auto* const self = window.GetData<Observation>();
    ++self->mRuns;
    self->mOnMain          = std::this_thread::get_id() == self->mMain;
    self->mInFlight        = self->mJobs != nullptr ? self->mJobs->GetStats().mInFlightJobs : 0;
    self->mLiveX           = window.Get<Position>(self->mEntity, self->mFixture->mTypes.mPosition)->mX;
    const auto* const prev = window.Prev<Position>(self->mEntity, self->mFixture->mTypes.mPosition);
    self->mPrevX           = prev != nullptr ? prev->mX : -1.0f;
    self->mVersion         = window.GetVersion();
    CHECK(window.GetPendingCommandCount() == 0);
}

struct Edits {
    Fixture* mFixture = nullptr;
    EntityId mKeep;
    EntityId mDoomed;
    uint32_t mRuns = 0;
};

void Edit(FirewallWindow& window) {
    auto* const self   = window.GetData<Edits>();
    const Types& types = self->mFixture->mTypes;
    if (self->mRuns++ > 0) {
        return;
    }
    // Nothing recorded here is visible inside the window.
    window.Set(self->mKeep, types.mPosition, Position{.mX = 10.0f, .mY = 0.0f, .mZ = 0.0f});
    CHECK(window.Get<Position>(self->mKeep, types.mPosition)->mX == 0.0f);
    window.DestroyEntity(self->mDoomed);
    CHECK(window.IsAlive(self->mDoomed));
    const PendingEntity born = window.CreateEntity(MaskOf({types.mPosition, types.mVelocity}));
    window.Set(born, types.mVelocity, Velocity{.mX = 5.0f, .mY = 0.0f, .mZ = 0.0f});
    window.Send(types.mNudge, self->mKeep, Nudge{.mX = 1.0f});
    window.Send(types.mNudge, self->mDoomed, Nudge{.mX = 100.0f}); // dropped: dead at routing
    CHECK(window.GetPendingCommandCount() == 4);
}

/// The ADR-0006 facade: "move this thing" without knowing how the thing moves.
class Mover : public Facade {
public:
    Mover(FirewallWindow& window, EntityId entity, const Types& types) noexcept
        : Facade(window, entity), mTypes(types) {}

    /// A velocity-driven entity gets a Nudge for its systems; a static one is repositioned.
    void Push(float dx) noexcept {
        if (Has(mTypes.mVelocity)) {
            Send(mTypes.mNudge, Nudge{.mX = dx});
            return;
        }
        const auto* const live = Get<Position>(mTypes.mPosition);
        Set(mTypes.mPosition, Position{.mX = live->mX + dx, .mY = 0.0f, .mZ = 0.0f});
    }

    [[nodiscard]] float GetX() const noexcept { return Get<Position>(mTypes.mPosition)->mX; }

private:
    const Types& mTypes;
};

/// A sync hook that creates an entity on its first run and counts the rest.
struct Hook {
    Fixture* mFixture = nullptr;
    uint32_t mRuns    = 0;
    uint32_t mSeen    = 0; ///< entities with Health when the hook last ran
};

void OnSync(FirewallWindow& window) {
    auto* const self = window.GetData<Hook>();
    if (self->mRuns++ == 0) {
        const PendingEntity born = window.CreateEntity(MaskOf({self->mFixture->mTypes.mHealth}));
        window.Set(born, self->mFixture->mTypes.mHealth, Health{.mCurrent = 7, .mMax = 7, .mLastAttacker = {}});
    }
    self->mSeen = 0;
    window.ForEachEntity(MaskOf({self->mFixture->mTypes.mHealth}), ComponentMask{},
                         [self](EntityId) { ++self->mSeen; });
}

struct Pushers {
    Fixture* mFixture = nullptr;
    EntityId mDriven;
    EntityId mStatic;
};

void PushBoth(FirewallWindow& window) {
    const auto* const self = window.GetData<Pushers>();
    Mover(window, self->mDriven, self->mFixture->mTypes).Push(1.0f);
    Mover(window, self->mStatic, self->mFixture->mTypes).Push(1.0f);
}

} // namespace

TEST_CASE("a window opens after the sync point, on the calling thread, with the world still",
          "[ecs][firewall][schedule]") {
    Fixture f;
    f.AddMovement();
    REQUIRE(f.mSchedule.Build().has_value());
    const EntityId entity = f.Spawn(2.0f);
    f.mSchedule.SetSnapshotCapture(&f.mPrev, MaskOf({f.mTypes.mPosition}), Phase::Variable);
    Observation seen;
    seen.mFixture = &f;
    seen.mEntity  = entity;
    f.mSchedule.SetWindow(Phase::Variable, &Observe, &seen);

    JobSystem jobs(f.mAllocator, kThreeWorkers);
    seen.mJobs = &jobs;
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(seen.mRuns == 1);
    CHECK(seen.mOnMain);
    CHECK(seen.mInFlight == 0);
    CHECK(seen.mLiveX == 2.0f); // the phase's writes are in
    CHECK(seen.mPrevX == 2.0f); // and so is the snapshot taken at its sync point
    CHECK(seen.mVersion == f.mWorld.GetVersion() - 1);

    // Phases without a window do not open one; the serial path opens the same window.
    REQUIRE(f.mSchedule.RunPhase(Phase::Begin, jobs).has_value());
    CHECK(seen.mRuns == 1);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(seen.mRuns == 2);
    CHECK(seen.mLiveX == 4.0f);
    f.mSchedule.SetWindow(Phase::Variable, nullptr, nullptr);
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(seen.mRuns == 2);
}

TEST_CASE("what a window records lands at its own sync point and reaches the systems", "[ecs][firewall][schedule]") {
    Fixture f;
    f.AddMovement();
    REQUIRE(f.mSchedule.Build().has_value());
    Edits edits;
    edits.mFixture = &f;
    edits.mKeep    = f.Spawn(0.0f);
    edits.mDoomed  = f.Spawn(0.0f);
    f.mSchedule.SetWindow(Phase::Variable, &Edit, &edits);

    JobSystem jobs(f.mAllocator, kThreeWorkers);
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(f.mSchedule.GetLastWindowReplayStats().mApplied == 4);
    CHECK(f.mSchedule.GetLastWindowReplayStats().mSkipped == 0);
    CHECK(f.mWorld.Get<Position>(edits.mKeep)->mX == 10.0f);
    CHECK_FALSE(f.mWorld.IsAlive(edits.mDoomed));
    CHECK(f.mWorld.GetEntityCount() == 2);

    // The next phase: nudge receives the window's message, move applies the new velocity; the
    // entity born in the window moves with its velocity.
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(f.mWorld.Get<Velocity>(edits.mKeep)->mX == 1.0f);
    CHECK(f.mWorld.Get<Position>(edits.mKeep)->mX == 11.0f);
    uint32_t born = 0;
    f.mWorld.ForEachChunk(MaskOf({f.mTypes.mPosition, f.mTypes.mVelocity}), MaskOf({f.mTypes.mHealth}),
                          [&](ChunkView& view) {
                              for (const Position& position : view.Read<Position>(f.mTypes.mPosition)) {
                                  CHECK(position.mX == 5.0f);
                                  ++born;
                              }
                          });
    CHECK(born == 1);
    CHECK(f.mSchedule.GetLastWindowReplayStats().mApplied == 0);
}

TEST_CASE("RunWindow opens a window on demand, even before Build", "[ecs][firewall][schedule]") {
    Fixture f;
    Edits edits;
    edits.mFixture = &f;
    edits.mKeep    = f.Spawn(0.0f);
    edits.mDoomed  = f.Spawn(0.0f);
    REQUIRE_FALSE(f.mSchedule.RunWindow(nullptr, nullptr).has_value());
    REQUIRE(f.mSchedule.RunWindow(&Edit, &edits).has_value());
    CHECK(f.mWorld.Get<Position>(edits.mKeep)->mX == 10.0f);
    CHECK_FALSE(f.mWorld.IsAlive(edits.mDoomed));
    CHECK(f.mWorld.GetEntityCount() == 2);
    CHECK(f.mSchedule.GetLastWindowReplayStats().mApplied == 4);
    // Nobody receives Nudge here; the message is dropped without a trace.
    f.AddMovement();
    REQUIRE(f.mSchedule.Build().has_value());
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(f.mWorld.Get<Velocity>(edits.mKeep)->mX == 0.0f);
}

TEST_CASE("a facade hides whether the entity is driven by systems or repositioned", "[ecs][firewall][facade]") {
    Fixture f;
    f.AddMovement();
    REQUIRE(f.mSchedule.Build().has_value());
    Pushers pushers;
    pushers.mFixture = &f;
    pushers.mDriven  = f.Spawn(0.0f);
    pushers.mStatic  = *f.mWorld.CreateEntity(MaskOf({f.mTypes.mPosition}));
    f.mSchedule.SetWindow(Phase::Variable, &PushBoth, &pushers);

    JobSystem jobs(f.mAllocator, kThreeWorkers);
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    // The static entity moved at the window's sync point; the driven one waits for its systems.
    CHECK(f.mWorld.Get<Position>(pushers.mStatic)->mX == 1.0f);
    CHECK(f.mWorld.Get<Position>(pushers.mDriven)->mX == 0.0f);
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(f.mWorld.Get<Position>(pushers.mStatic)->mX == 2.0f);
    CHECK(f.mWorld.Get<Position>(pushers.mDriven)->mX == 1.0f);
    CHECK(f.mWorld.Get<Velocity>(pushers.mDriven)->mX == 1.0f);
}

TEST_CASE("frames with a window match between the serial and the parallel run", "[ecs][firewall][schedule][job]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        f.AddMovement();
        REQUIRE(f.mSchedule.Build().has_value());
        std::vector<EntityId> entities;
        entities.reserve(400);
        for (uint32_t i = 0; i < 400; ++i) {
            entities.push_back(f.Spawn(static_cast<float>(i % 3)));
        }
        struct Pusher {
            Fixture* mFixture = nullptr;
            uint32_t mFrame   = 0;
        } pusher{.mFixture = &f, .mFrame = 0};
        const auto push = [](FirewallWindow& window) {
            auto* const self   = window.GetData<Pusher>();
            const Types& types = self->mFixture->mTypes;
            uint32_t index     = 0;
            window.ForEachEntity(MaskOf({types.mPosition, types.mVelocity}), ComponentMask{}, [&](EntityId entity) {
                if ((index++ + self->mFrame) % 2 == 0) {
                    window.Send(types.mNudge, entity, Nudge{.mX = 0.5f});
                } else {
                    window.Set(entity, types.mHealth, Health{.mCurrent = 7, .mMax = 100, .mLastAttacker = {}});
                }
            });
            ++self->mFrame;
        };
        f.mSchedule.SetWindow(Phase::Variable, push, &pusher);
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
        std::vector<float> state;
        for (const EntityId entity : entities) {
            state.push_back(f.mWorld.Get<Position>(entity)->mX);
            state.push_back(f.mWorld.Get<Velocity>(entity)->mX);
            state.push_back(static_cast<float>(f.mWorld.Get<Health>(entity)->mCurrent));
        }
        return state;
    };
    const auto serial   = simulate(false);
    const auto parallel = simulate(true);
    CHECK(serial.size() == 1200);
    CHECK(serial == parallel);
}

TEST_CASE("the sync hook opens after every sync point and its records apply before anything else runs",
          "[ecs][firewall][schedule][object]") {
    Fixture f;
    f.AddMovement();
    REQUIRE(f.mSchedule.Build().has_value());
    Hook hook;
    hook.mFixture = &f;
    f.mSchedule.SetSyncHook(&OnSync, &hook);
    (void)f.Spawn(1.0f); // has Health too

    // The phase's sync point opens the hook; what the hook recorded is live right after.
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(hook.mRuns == 1);
    CHECK(hook.mSeen == 1); // the hook's own entity is not there while it runs
    CHECK(f.mSchedule.GetSyncHookRunCount() == 1);
    CHECK(f.mWorld.GetEntityCount() == 2);
    // The hook's replay does not open the hook again; an explicit window does, after its own sync point.
    Observation seen;
    seen.mFixture = &f;
    seen.mEntity  = f.Spawn(0.0f);
    REQUIRE(f.mSchedule.RunWindow(&Observe, &seen).has_value());
    CHECK(hook.mRuns == 2);
    CHECK(hook.mSeen == 3);
    // A per-phase window: the phase's sync point, then the window's.
    f.mSchedule.SetWindow(Phase::Variable, &Observe, &seen);
    JobSystem jobs(f.mAllocator, kThreeWorkers);
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(hook.mRuns == 4);
    f.mSchedule.SetSyncHook(nullptr, nullptr);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(hook.mRuns == 4);
}
