#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_job/JobSystem.h>
#include <oyi_script/ScriptVm.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::FirewallWindow;
using oyi::ecs::ManagedContext;
using oyi::ecs::ManagedId;
using oyi::ecs::ManagedObject;
using oyi::ecs::MessageMask;
using oyi::ecs::MessageTypeIndex;
using oyi::ecs::Phase;
using oyi::ecs::ResourceMask;
using oyi::ecs::Schedule;
using oyi::ecs::Snapshot;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::SystemId;
using oyi::ecs::World;
using oyi::job::JobSystem;
using oyi::script::ScriptModuleId;
using oyi::script::ScriptVm;
using namespace oyi::ecs_fixture;

constexpr JobSystem::Config kThreeWorkers{
    .mWorkerCount = 3, .mMaxJobs = 256, .mMaxCounters = 128, .mSpinIterations = 8};
constexpr ScriptVm::Config kVmConfig{.mMemoryLimit = size_t{4} * 1024 * 1024, .mCallBudgetMicroseconds = 50000};

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

[[nodiscard]] bool Contains(std::string_view text, std::string_view needle) {
    return text.contains(needle);
}

struct Types {
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mVelocity;
    ComponentTypeIndex mHealth;
    ComponentTypeIndex mTransform;
    ComponentTypeIndex mBrainRef;
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

[[nodiscard]] SystemDesc Describe(std::string_view name, ComponentMask read, ComponentMask write, MessageMask receives,
                                  oyi::ecs::SystemFunction function, void* data) {
    return SystemDesc{.mName           = name,
                      .mPhase          = Phase::Variable,
                      .mRead           = read,
                      .mWrite          = write,
                      .mExclude        = ComponentMask{},
                      .mOptional       = ComponentMask{},
                      .mSends          = MessageMask{},
                      .mReceives       = receives,
                      .mReadResources  = ResourceMask{},
                      .mWriteResources = ResourceMask{},
                      .mMainThread     = false,
                      .mFunction       = function,
                      .mData           = data};
}

/// A managed type scripts can address: its state block is the fixture's BrainState.
class ScriptBrain final : public ManagedObject {
    OYI_MANAGED(ScriptBrain, BrainState, oyi::ecs::Access<>)
public:
    void Update(ManagedContext& /*context*/) override { ++mState.mUpdates; }
};

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    Types mTypes{.mPosition  = *mWorld.GetComponents().Register<Position>(),
                 .mVelocity  = *mWorld.GetComponents().Register<Velocity>(),
                 .mHealth    = *mWorld.GetComponents().Register<Health>(),
                 .mTransform = *mWorld.GetComponents().Register<Transform>(),
                 .mBrainRef  = *mWorld.GetComponents().Register<BrainRef>(),
                 .mNudge     = *mWorld.GetMessages().Register<Nudge>()};
    Snapshot mPrev{mAllocator};
    Schedule mSchedule{mAllocator, mWorld};
    ScriptVm mVm{mAllocator, mWorld, kVmConfig};

    Fixture() {
        const SystemId nudge = *mSchedule.AddSystem(Describe("nudge", {}, MaskOf({mTypes.mVelocity}),
                                                             MessageMask{}.With(mTypes.mNudge), &NudgeSystem, &mTypes));
        const SystemId move  = *mSchedule.AddSystem(
            Describe("move", MaskOf({mTypes.mVelocity}), MaskOf({mTypes.mPosition}), {}, &MoveSystem, &mTypes));
        REQUIRE(mSchedule.AddOrder(nudge, move).has_value());
        REQUIRE(mSchedule.Build().has_value());
        mSchedule.SetSnapshotCapture(&mPrev, MaskOf({mTypes.mPosition}), Phase::Variable);
        REQUIRE(mWorld.RegisterManaged<ScriptBrain>().has_value());
    }
};

/// The window function: one script call per window.
struct ScriptTick {
    ScriptVm* mVm = nullptr;
    ScriptModuleId mModule;
    const char* mFunction = "tick";
    oyi::core::Result mResult;
};

void RunScript(FirewallWindow& window) {
    auto* const tick = window.GetData<ScriptTick>();
    tick->mResult    = tick->mVm->Call(window, tick->mModule, tick->mFunction);
}

constexpr std::string_view kGameplay = R"lua(
    local Position = oyi.component("oyi::ecs_fixture::Position")
    local Velocity = oyi.component("oyi::ecs_fixture::Velocity")
    local Health = oyi.component("oyi::ecs_fixture::Health")
    local Nudge = oyi.message("oyi::ecs_fixture::Nudge")
    ticks = 0

    function tick()
        ticks += 1
        local env = oyi.env()
        assert(env.fixed_step > 0 and env.frame == 7, "environment")
        -- Static things are repositioned by hand ...
        for _, e in ipairs(oyi.entities({Position}, {Velocity})) do
            local p = oyi.get(e, Position)
            oyi.set(e, Position, { mX = p.mX + 1 })
        end
        -- ... driven things are asked to move through their systems.
        for _, e in ipairs(oyi.entities({Position, Velocity})) do
            assert(oyi.is_alive(e) and oyi.has(e, Velocity), "handle")
            oyi.send(Nudge, e, { mX = 0.5 })
            local h = oyi.get(e, Health)
            if h ~= nil and h.mCurrent == 100 then
                oyi.set(e, Health, { mCurrent = 90, mLastAttacker = e })
            end
            local prev = oyi.prev(e, Position)
            if prev ~= nil then
                assert(prev.mX == oyi.get(e, Position).mX, "prev equals live right after the sync point")
            end
        end
        if ticks == 1 then
            local born = oyi.create({Position, Health})
            oyi.set(born, Position, { mX = 42, mY = 1 })
            oyi.set(born, Health, { mCurrent = 7 })
        end
    end

    function cull()
        for _, e in ipairs(oyi.entities({Health})) do
            if oyi.get(e, Health).mCurrent < 10 then oyi.destroy(e) end
        end
        for _, e in ipairs(oyi.entities({Position}, {Velocity})) do
            oyi.add(e, Velocity)
        end
    end
)lua";

} // namespace

TEST_CASE("scripts read live and Prev data and write through the window", "[script][binding][schedule]") {
    Fixture f;
    const EntityId drivenA = *f.mWorld.CreateEntity(MaskOf({f.mTypes.mPosition, f.mTypes.mVelocity, f.mTypes.mHealth}));
    const EntityId drivenB = *f.mWorld.CreateEntity(MaskOf({f.mTypes.mPosition, f.mTypes.mVelocity, f.mTypes.mHealth}));
    const EntityId still   = *f.mWorld.CreateEntity(MaskOf({f.mTypes.mPosition}));
    f.mWorld.GetEnvironmentForWrite().mFrameIndex = 7;
    ScriptTick tick{.mVm = &f.mVm, .mModule = *f.mVm.Load("gameplay", kGameplay), .mFunction = "tick", .mResult = {}};
    f.mSchedule.SetWindow(Phase::Variable, &RunScript, &tick);

    JobSystem jobs(f.mAllocator, kThreeWorkers);
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    INFO(f.mVm.GetLastError());
    REQUIRE(tick.mResult.has_value());
    CHECK(f.mWorld.Get<Position>(still)->mX == 1.0f);
    CHECK(f.mWorld.Get<Health>(drivenA)->mCurrent == 90);
    CHECK(f.mWorld.Get<Health>(drivenA)->mLastAttacker == drivenA);
    CHECK(f.mWorld.Get<Health>(drivenB)->mLastAttacker == drivenB);
    CHECK(f.mWorld.GetEntityCount() == 4);
    CHECK(f.mWorld.Get<Velocity>(drivenA)->mX == 0.0f); // the Nudge is delivered next phase

    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    INFO(f.mVm.GetLastError());
    REQUIRE(tick.mResult.has_value());
    CHECK(f.mWorld.Get<Position>(still)->mX == 2.0f);
    CHECK(f.mWorld.Get<Velocity>(drivenA)->mX == 0.5f);
    CHECK(f.mWorld.Get<Position>(drivenA)->mX == 0.5f);
    CHECK(f.mWorld.Get<Health>(drivenA)->mCurrent == 90);
    CHECK(f.mWorld.GetEntityCount() == 4);
    uint32_t born = 0;
    f.mWorld.ForEachChunk(MaskOf({f.mTypes.mPosition, f.mTypes.mHealth}), MaskOf({f.mTypes.mVelocity}),
                          [&](ChunkView& view) {
                              const std::span<const Position> positions = view.Read<Position>(f.mTypes.mPosition);
                              const std::span<const Health> healths     = view.Read<Health>(f.mTypes.mHealth);
                              for (size_t i = 0; i < positions.size(); ++i) {
                                  // Born at 42 in tick 1, pushed like any static entity in tick 2.
                                  CHECK(positions[i].mX == 43.0f);
                                  CHECK(positions[i].mY == 1.0f);
                                  CHECK(healths[i].mCurrent == 7);
                                  ++born;
                              }
                          });
    CHECK(born == 1);

    // Structural changes on demand, from a window opened by hand.
    tick.mFunction = "cull";
    REQUIRE(f.mSchedule.RunWindow(&RunScript, &tick).has_value());
    INFO(f.mVm.GetLastError());
    REQUIRE(tick.mResult.has_value());
    CHECK(f.mWorld.GetEntityCount() == 3);
    CHECK(f.mWorld.HasComponent(still, f.mTypes.mVelocity));
}

TEST_CASE("the oyi library refuses to touch entities outside a window", "[script][binding]") {
    Fixture f;
    const auto outside = f.mVm.Load("outside", R"lua(
        local Position = oyi.component("oyi::ecs_fixture::Position")
        oyi.entities({Position})
    )lua");
    REQUIRE_FALSE(outside.has_value());
    CHECK(Contains(f.mVm.GetLastError(), "firewall window"));
    CHECK_FALSE(f.mVm.Load("unknown", "oyi.component('nope')").has_value());
    CHECK(Contains(f.mVm.GetLastError(), "unknown component"));
    CHECK_FALSE(f.mVm.Load("badmsg", "oyi.message('nope')").has_value());
}

TEST_CASE("handles are opaque, arrays and foreign handles round-trip through tables", "[script][binding]") {
    Fixture f;
    const EntityId entity = *f.mWorld.CreateEntity(MaskOf({f.mTypes.mTransform, f.mTypes.mBrainRef}));
    f.mWorld.GetForWrite<BrainRef>(entity)->mObject = ManagedId{.mIndex = 5, .mGeneration = 9};
    ScriptTick tick{.mVm       = &f.mVm,
                    .mModule   = *f.mVm.Load("handles", R"lua(
        local Transform = oyi.component("oyi::ecs_fixture::Transform")
        local BrainRef = oyi.component("oyi::ecs_fixture::BrainRef")
        local Position = oyi.component("oyi::ecs_fixture::Position")
        function tick()
            local e = oyi.entities({Transform})[1]
            assert(string.find(oyi.entity_tostring(e), "^entity%(%d+:%d+%)$"), "tostring")
            local t = oyi.get(e, Transform)
            assert(#t.mMatrix == 16 and t.mMatrix[1] == 1 and t.mMatrix[2] == 0, "array read")
            oyi.set(e, Transform, { mMatrix = { 2, 3 } })
            local r = oyi.get(e, BrainRef)
            assert(type(r.mObject) == "userdata", "foreign handle is opaque")
            oyi.set(e, BrainRef, { mObject = r.mObject })
            assert(oyi.get(e, Position) == nil, "absent component")
            local ok, err = pcall(oyi.get, 12, Position)
            assert(not ok and string.find(err, "entity expected"), "type check")
        end
    )lua"),
                    .mFunction = "tick",
                    .mResult   = {}};
    REQUIRE(f.mSchedule.RunWindow(&RunScript, &tick).has_value());
    INFO(f.mVm.GetLastError());
    REQUIRE(tick.mResult.has_value());
    const auto* const transform = f.mWorld.Get<Transform>(entity);
    CHECK(transform->mMatrix[0] == 2.0f);
    CHECK(transform->mMatrix[1] == 3.0f);
    CHECK(transform->mMatrix[2] == 0.0f);
    CHECK(transform->mMatrix[5] == 1.0f);
    CHECK(f.mWorld.Get<BrainRef>(entity)->mObject == ManagedId{.mIndex = 5, .mGeneration = 9});
}

TEST_CASE("scripts read and write managed state blocks by type name", "[script][binding][object]") {
    Fixture f;
    const EntityId owner = *f.mWorld.CreateEntity(MaskOf({f.mTypes.mPosition}));
    const EntityId bare  = *f.mWorld.CreateEntity(MaskOf({f.mTypes.mPosition, f.mTypes.mHealth}));
    const ManagedId id   = *f.mWorld.GetManaged().Create<ScriptBrain>(owner);
    f.mWorld.GetManaged().Resolve<ScriptBrain>(id)->GetStateForWrite().mUpdates = 4;
    ScriptTick tick{.mVm       = &f.mVm,
                    .mModule   = *f.mVm.Load("managed", R"lua(
        local Brain = oyi.managed("oyi::ecs_fixture::BrainState")
        local Position = oyi.component("oyi::ecs_fixture::Position")
        local Health = oyi.component("oyi::ecs_fixture::Health")
        function tick()
            local owner, bare
            for _, e in ipairs(oyi.entities({Position})) do
                if oyi.has(e, Health) then bare = e else owner = e end
            end
            local s = oyi.state(owner, Brain)
            assert(s.mUpdates == 4 and s.mSawPrevX == -2, "state read")
            oyi.set_state(owner, Brain, { mSawPrevX = 2.5 })
            assert(oyi.state(owner, Brain).mSawPrevX == -2, "writes are deferred")
            assert(oyi.state(bare, Brain) == nil, "no object, no state")
            local ok, err = pcall(oyi.set_state, bare, Brain, {})
            assert(not ok and string.find(err, "no managed object"), "write without an object")
            local ok2, err2 = pcall(oyi.managed, "nope")
            assert(not ok2 and string.find(err2, "unknown managed type"), "unknown type")
            local ok3, err3 = pcall(oyi.state, owner, 7)
            assert(not ok3 and string.find(err3, "unknown managed type"), "bad handle")
        end
    )lua"),
                    .mFunction = "tick",
                    .mResult   = {}};
    REQUIRE(f.mSchedule.RunWindow(&RunScript, &tick).has_value());
    INFO(f.mVm.GetLastError());
    REQUIRE(tick.mResult.has_value());
    CHECK(f.mWorld.GetManaged().Resolve<ScriptBrain>(id)->GetState().mSawPrevX == 2.5f);
    CHECK(f.mWorld.GetManaged().Resolve<ScriptBrain>(id)->GetState().mUpdates == 4); // absent fields keep their value
    CHECK(f.mWorld.IsAlive(bare));
}
