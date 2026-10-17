#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/Builders.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_object/Components.gen.h> // IWYU pragma: keep
#include <oyi_object/Components.h>
#include <oyi_object/ObjectFactory.h>
#include <oyi_object/ObjectLayer.h>
#include <oyi_object/Prototype.h>
#include <oyi_script/ScriptManaged.h>
#include <oyi_script/ScriptVm.h>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentOf;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::FirewallWindow;
using oyi::ecs::ManagedId;
using oyi::ecs::ManagedSystem;
using oyi::ecs::ManagedTypeInfo;
using oyi::ecs::MessageOf;
using oyi::ecs::MessageTypeIndex;
using oyi::ecs::Phase;
using oyi::ecs::RegisterModule;
using oyi::ecs::Schedule;
using oyi::ecs::Snapshot;
using oyi::ecs::System;
using oyi::ecs::SystemContext;
using oyi::ecs::World;
using oyi::object::ManagedBinding;
using oyi::object::ObjectFactory;
using oyi::object::ObjectLayer;
using oyi::object::ObjectTypes;
using oyi::object::PrototypeDesc;
using oyi::object::PrototypeId;
using oyi::object::RegisterObjectTypes;
using oyi::script::ScriptManagedObject;
using oyi::script::ScriptManagedType;
using oyi::script::ScriptModuleId;
using oyi::script::ScriptVm;
using namespace oyi::ecs_fixture;

constexpr ScriptVm::Config kVmConfig{.mMemoryLimit = size_t{4} * 1024 * 1024, .mCallBudgetMicroseconds = 2000000};

[[nodiscard]] bool Contains(std::string_view text, std::string_view needle) {
    return text.contains(needle);
}

/// What a receiving system collected: Damage messages with their targets.
struct Seen {
    std::vector<EntityId> mTargets;
    std::vector<float> mAmounts;
    MessageTypeIndex mDamage;
};

// The signature is SystemFunction's; the context is only read.
void ReceiverSystem(SystemContext& context) { // NOLINT(misc-const-correctness)
    auto* const seen    = context.GetData<Seen>();
    const auto received = context.Received<Damage>(seen->mDamage);
    for (size_t i = 0; i < received.mTargets.size(); ++i) {
        seen->mTargets.push_back(received.mTargets[i]);
        seen->mAmounts.push_back(received.mPayloads[i].mAmount);
    }
}

constexpr std::string_view kBrain = R"lua(
    local Position = oyi.component("oyi::ecs_fixture::Position")
    local Health = oyi.component("oyi::ecs_fixture::Health")
    local Nudge = oyi.message("oyi::ecs_fixture::Nudge")
    local Damage = oyi.message("oyi::ecs_fixture::Damage")
    Brain = oyi.managed_type("oyi::ecs_fixture::BrainState",
        { reads = {Position}, writes = {Health}, receives = {Nudge}, sends = {Damage} })
    function update(self, ctx)
        self.mUpdates += 1
        local p = ctx.get(Position)
        self.mSawPrevX = p.mX
        local prev = ctx.prev(Position)
        if prev ~= nil then self.mSawPrevX = prev.mX + 100 end
        local h = ctx.get(Health)
        for _, n in ipairs(ctx.received(Nudge)) do
            ctx.set(Health, { mCurrent = h.mCurrent - n.mX })
            ctx.send(Damage, ctx.owner(), { mAmount = n.mX })
        end
        assert(ctx.env().fixed_step > 0, "env")
    end
    function on_spawned(self, owner)
        self.mUpdates = 100
        assert(oyi.is_alive(owner), "owner alive in on_spawned")
    end
    function on_despawning(self, owner)
        oyi.send(Damage, owner, { mAmount = 7 })
    end
)lua";

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ObjectTypes mObject;
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mHealth;
    ComponentTypeIndex mBrainRef;
    MessageTypeIndex mNudge;
    MessageTypeIndex mDamage;
    Snapshot mPrev{mAllocator};
    Schedule mSchedule{mAllocator, mWorld};
    ScriptVm mVm{mAllocator, mWorld, kVmConfig};
    Seen mSeen;

    Fixture() {
        REQUIRE(RegisterModule(mWorld, kModule).has_value());
        mObject       = *RegisterObjectTypes(mWorld);
        mPosition     = ComponentOf<Position>(mWorld);
        mHealth       = ComponentOf<Health>(mWorld);
        mBrainRef     = ComponentOf<BrainRef>(mWorld);
        mNudge        = MessageOf<Nudge>(mWorld);
        mDamage       = MessageOf<Damage>(mWorld);
        mSeen.mDamage = mDamage;
        mVm.RegisterStruct<BrainState>();
        REQUIRE(mVm.IsValid());
    }

    /// Loads the module, finds the type it declared, and schedules its managed system and a
    /// Damage receiver. The brains run in Variable with the Prev snapshot of that phase.
    [[nodiscard]] const ScriptManagedType* Arm(std::string_view source) {
        const auto module = mVm.Load("brain", source);
        INFO(mVm.GetLastError());
        REQUIRE(module.has_value());
        const ScriptManagedType* const type =
            mVm.FindManagedType(oyi::core::ComponentTraits<BrainState>::kInfo.mTypeId);
        REQUIRE(type != nullptr);
        return type;
    }

    void Build(const ManagedTypeInfo& info, ManagedSystem& brains) {
        REQUIRE(mSchedule
                    .AddSystem(*brains.Describe(
                        *System("brains", mWorld).Phase(Phase::Variable).ReadsEnvironment().Build(), info, mWorld))
                    .has_value());
        REQUIRE(
            mSchedule
                .AddSystem(*System("receiver", mWorld).Phase(Phase::End).Receives<Damage>().Fn(&ReceiverSystem, &mSeen))
                .has_value());
        REQUIRE(mSchedule.Build().has_value());
        mSchedule.SetSnapshotCapture(&mPrev, ComponentMask{}.With(mPosition), Phase::Variable);
    }

    /// An entity with a scripted brain made the way the object layer does: through mCreate.
    [[nodiscard]] EntityId Spawn(const ManagedTypeInfo& info) {
        const EntityId entity = *mWorld.CreateEntity(ComponentMask{}.With(mPosition).With(mHealth).With(mBrainRef));
        oyi::ecs::ManagedObject* const object = info.mCreate(mWorld.GetManaged().GetAllocator(), info);
        REQUIRE(object != nullptr);
        const ManagedId id                            = *mWorld.GetManaged().Adopt(object, entity);
        mWorld.GetForWrite<BrainRef>(entity)->mObject = id;
        return entity;
    }

    [[nodiscard]] const BrainState& StateOf(EntityId entity) {
        const auto* const object = mWorld.GetManaged().Resolve(mWorld.Get<BrainRef>(entity)->mObject);
        REQUIRE(object != nullptr);
        return *static_cast<const BrainState*>(object->GetStateBytes());
    }
};

struct NudgeInWindow {
    MessageTypeIndex mNudge;
    EntityId mTarget;
    float mAmount = 0.0f;
};

void SendNudge(FirewallWindow& window) {
    const auto* const self = window.GetData<NudgeInWindow>();
    window.Send(self->mNudge, self->mTarget, Nudge{.mX = self->mAmount});
}

} // namespace

TEST_CASE("a managed type declared from Luau runs its update in the managed system group",
          "[script][managed][object]") {
    Fixture f;
    const ScriptManagedType* const type = f.Arm(kBrain);
    const ManagedTypeInfo& info         = type->GetInfo();
    CHECK(info.GetName() == "oyi::ecs_fixture::BrainState");
    CHECK(info.GetStateSize() == sizeof(BrainState));
    CHECK(info.mReads.size() == 1);
    CHECK(info.mWrites.size() == 1);
    CHECK(info.mReceives.size() == 1);
    CHECK(info.mSends.size() == 1);
    CHECK(info.mUserData == type);
    CHECK(type->GetModule() == f.mVm.Find("brain"));
    CHECK(f.mWorld.GetManagedTypes().Find(info.GetTypeId()) == &info);
    CHECK(f.mWorld.GetManagedTypes().Find("oyi::ecs_fixture::BrainState") == &info);

    ManagedSystem brains(f.mWorld.GetManaged(), f.mBrainRef, &f.mPrev);
    f.Build(info, brains);
    const EntityId entity                          = f.Spawn(info);
    f.mWorld.GetForWrite<Position>(entity)->mX     = 3.0f;
    f.mWorld.GetForWrite<Health>(entity)->mCurrent = 50;
    CHECK(f.StateOf(entity).mUpdates == 0); // the state block starts at its defaults
    CHECK(f.StateOf(entity).mSawPrevX == -2.0f);

    // First run: no Prev yet, the live Position is read; second run: Prev is there.
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    INFO(f.mVm.GetLastError());
    CHECK(type->GetErrorCount() == 0);
    CHECK(brains.GetLastUpdateCount() == 1);
    CHECK(f.StateOf(entity).mUpdates == 1);
    CHECK(f.StateOf(entity).mSawPrevX == 3.0f);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(f.StateOf(entity).mUpdates == 2);
    CHECK(f.StateOf(entity).mSawPrevX == 103.0f);

    // A Nudge from a window: update writes Health and sends Damage to its owner.
    NudgeInWindow nudge{.mNudge = f.mNudge, .mTarget = entity, .mAmount = 4.0f};
    REQUIRE(f.mSchedule.RunWindow(&SendNudge, &nudge).has_value());
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    INFO(f.mVm.GetLastError());
    CHECK(type->GetErrorCount() == 0);
    CHECK(f.mWorld.Get<Health>(entity)->mCurrent == 46);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::End).has_value());
    REQUIRE(f.mSeen.mAmounts.size() == 1);
    CHECK(f.mSeen.mAmounts[0] == 4.0f);
    CHECK(f.mSeen.mTargets[0] == entity);

    // The state block is what oyi.state shows a window script.
    const auto probe = f.mVm.Load("probe", R"lua(
        local Brain = oyi.managed("oyi::ecs_fixture::BrainState")
        local Position = oyi.component("oyi::ecs_fixture::Position")
        function tick()
            local e = oyi.entities({Position})[1]
            assert(oyi.state(e, Brain).mUpdates == 3, "state visible")
            oyi.set_state(e, Brain, { mUpdates = 10 })
        end
    )lua");
    REQUIRE(probe.has_value());
    struct Tick {
        ScriptVm* mVm;
        ScriptModuleId mModule;
    } tick{.mVm = &f.mVm, .mModule = *probe};
    REQUIRE(f.mSchedule
                .RunWindow(
                    [](FirewallWindow& window) {
                        const auto* const self = window.GetData<Tick>();
                        REQUIRE(self->mVm->Call(window, self->mModule, "tick").has_value());
                    },
                    &tick)
                .has_value());
    CHECK(f.StateOf(entity).mUpdates == 10);
}

TEST_CASE("undeclared access from update is an error that leaves the VM usable; a reload takes over",
          "[script][managed][object]") {
    Fixture f;
    const ScriptManagedType* const type = f.Arm(R"lua(
        local Position = oyi.component("oyi::ecs_fixture::Position")
        local Health = oyi.component("oyi::ecs_fixture::Health")
        oyi.managed_type("oyi::ecs_fixture::BrainState", { reads = {Position} })
        function update(self, ctx)
            self.mUpdates += 1
            ctx.get(Health) -- not declared
        end
    )lua");
    ManagedSystem brains(f.mWorld.GetManaged(), f.mBrainRef, nullptr);
    f.Build(type->GetInfo(), brains);
    const EntityId entity = f.Spawn(type->GetInfo());
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(type->GetErrorCount() == 1);
    CHECK(Contains(f.mVm.GetLastError(), "not declared"));
    CHECK(f.StateOf(entity).mUpdates == 0); // a failed update writes nothing back

    // Other misuse: ctx outside update, managed_type inside a function, unknown state block.
    const auto outside = f.mVm.Load("outside", R"lua(
        local ok, err = pcall(oyi.managed_type, "nope", {})
        assert(not ok and string.find(err, "unknown state block"), err)
        function later()
            oyi.managed_type("oyi::ecs_fixture::BrainState", {})
        end
    )lua");
    REQUIRE(outside.has_value());
    struct Tick {
        ScriptVm* mVm;
        ScriptModuleId mModule;
    } tick{.mVm = &f.mVm, .mModule = *outside};
    REQUIRE(f.mSchedule
                .RunWindow(
                    [](FirewallWindow& window) {
                        const auto* const self = window.GetData<Tick>();
                        CHECK_FALSE(self->mVm->Call(window, self->mModule, "later").has_value());
                    },
                    &tick)
                .has_value());
    CHECK(Contains(f.mVm.GetLastError(), "top level"));

    // Reloading the module redefines the type in place: the same object runs the new update.
    const ScriptManagedType* const reloaded = f.Arm(R"lua(
        local Position = oyi.component("oyi::ecs_fixture::Position")
        oyi.managed_type("oyi::ecs_fixture::BrainState", { reads = {Position} })
        function update(self, ctx)
            self.mUpdates += 10
            self.mSawPrevX = ctx.get(Position).mX * 2
        end
    )lua");
    CHECK(reloaded == type);
    f.mWorld.GetForWrite<Position>(entity)->mX = 4.0f;
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    INFO(f.mVm.GetLastError());
    CHECK(type->GetErrorCount() == 1);
    CHECK(f.StateOf(entity).mUpdates == 10);
    CHECK(f.StateOf(entity).mSawPrevX == 8.0f);

    // A reload may not widen its access past what the schedule built: the system's gate holds.
    const ScriptManagedType* const widened = f.Arm(R"lua(
        local Position = oyi.component("oyi::ecs_fixture::Position")
        local Health = oyi.component("oyi::ecs_fixture::Health")
        oyi.managed_type("oyi::ecs_fixture::BrainState", { reads = {Position, Health} })
        function update(self, ctx)
            self.mUpdates += 1
            ctx.get(Health)
        end
    )lua");
    CHECK(widened == type);
    CHECK(widened->GetReads().Test(f.mHealth));
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(type->GetErrorCount() == 2);
    CHECK(Contains(f.mVm.GetLastError(), "not declared"));
    CHECK(f.StateOf(entity).mUpdates == 10);
}

TEST_CASE("the object layer spawns scripted objects from a prototype and runs their hooks",
          "[script][managed][object]") {
    Fixture f;
    const ScriptManagedType* const type = f.Arm(kBrain);
    ObjectLayer layer(f.mAllocator, f.mWorld, f.mObject);
    REQUIRE(layer.Install(f.mSchedule).has_value());
    const std::array<ManagedBinding, 1> managed{ManagedBinding{.mType = &type->GetInfo(), .mReference = f.mBrainRef}};
    const PrototypeId door = *layer.GetPrototypes().Register(
        PrototypeDesc{.mName    = "door",
                      .mMask    = ComponentMask{}.With(f.mPosition).With(f.mHealth).With(f.mBrainRef),
                      .mManaged = managed});
    ManagedSystem brains(f.mWorld.GetManaged(), f.mBrainRef, nullptr);
    f.Build(type->GetInfo(), brains);

    struct Spawn {
        ObjectLayer* mLayer;
        PrototypeId mDoor;
        EntityId mDespawn;
    } spawn{.mLayer = &layer, .mDoor = door, .mDespawn = EntityId::Invalid()};
    const auto act = [](FirewallWindow& window) {
        const auto* const self = window.GetData<Spawn>();
        ObjectFactory factory(*self->mLayer, window);
        if (self->mDespawn.IsValid()) {
            factory.Despawn(self->mDespawn);
        } else {
            (void)factory.Spawn(self->mDoor);
        }
    };
    REQUIRE(f.mSchedule.RunWindow(act, &spawn).has_value());
    INFO(f.mVm.GetLastError());
    CHECK(type->GetErrorCount() == 0);
    CHECK(f.mWorld.GetManaged().GetLiveCount() == 1);
    EntityId entity;
    f.mWorld.ForEachChunk(ComponentMask{}.With(f.mBrainRef), ComponentMask{},
                          [&](oyi::ecs::ChunkView& view) { entity = view.GetEntities()[0]; });
    REQUIRE(entity.IsValid());
    CHECK(f.StateOf(entity).mUpdates == 100); // on_spawned ran with self read back
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(f.StateOf(entity).mUpdates == 101);

    spawn.mDespawn = entity;
    REQUIRE(f.mSchedule.RunWindow(act, &spawn).has_value());
    INFO(f.mVm.GetLastError());
    CHECK(type->GetErrorCount() == 0);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::End).has_value()); // on_despawning's Damage arrives; then the entity dies
    REQUIRE(f.mSeen.mAmounts.size() == 1);
    CHECK(f.mSeen.mAmounts[0] == 7.0f);
    CHECK_FALSE(f.mWorld.IsAlive(entity));
    CHECK(f.mWorld.GetManaged().GetLiveCount() == 0);
}
