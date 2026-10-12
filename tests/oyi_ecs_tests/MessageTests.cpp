#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_core/TypeId.h>
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
using oyi::ecs::MessageBuffer;
using oyi::ecs::MessageInbox;
using oyi::ecs::MessageMask;
using oyi::ecs::MessageRegistry;
using oyi::ecs::MessageTypeIndex;
using oyi::ecs::Phase;
using oyi::ecs::Schedule;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::SystemId;
using oyi::ecs::World;
using oyi::job::JobSystem;
using namespace oyi::ecs_fixture;

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

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    MessageTypeIndex mDamage     = *mWorld.GetMessages().Register<Damage>();
    MessageTypeIndex mPing       = *mWorld.GetMessages().Register<Ping>();
    Schedule mSchedule{mAllocator, mWorld};
};

struct Types {
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mHealth;
    MessageTypeIndex mDamage;
    MessageTypeIndex mPing;
};

/// Sends one Damage to every entity it sees plus a Ping broadcast per run.
struct Attacker {
    Types mTypes;
    uint32_t mRuns = 0;

    static void Run(SystemContext& context) {
        auto* const self = context.GetData<Attacker>();
        ++self->mRuns;
        context.ForEachChunk([&](ChunkView& view) {
            for (const EntityId target : view.GetEntities()) {
                context.Send(self->mTypes.mDamage, target,
                             Damage{.mSource = EntityId::Invalid(), .mAmount = static_cast<float>(target.mIndex)});
            }
        });
        context.Send(self->mTypes.mPing, EntityId::Invalid(), Ping{.mSerial = self->mRuns});
    }
};

/// Applies received Damage to Health and counts Pings.
struct Defender {
    Types mTypes;
    uint32_t mDamageSeen = 0;
    uint32_t mPingsSeen  = 0;
    uint32_t mLastSerial = 0;
    std::vector<float> mAmounts;

    static void Run(SystemContext& context) {
        auto* const self  = context.GetData<Defender>();
        const auto damage = context.Received<Damage>(self->mTypes.mDamage);
        for (size_t i = 0; i < damage.mPayloads.size(); ++i) {
            ++self->mDamageSeen;
            self->mAmounts.push_back(damage.mPayloads[i].mAmount);
            auto* const health = context.LookupForWrite<Health>(self->mTypes.mHealth, damage.mTargets[i]);
            if (health != nullptr) {
                health->mCurrent = static_cast<uint16_t>(health->mCurrent - 1);
            }
        }
        for (const Ping& ping : context.Received<Ping>(self->mTypes.mPing).mPayloads) {
            ++self->mPingsSeen;
            self->mLastSerial = ping.mSerial;
        }
    }
};

[[nodiscard]] SystemDesc Describe(std::string_view name, Phase phase, ComponentMask read, ComponentMask write,
                                  MessageMask sends, MessageMask receives, oyi::ecs::SystemFunction function,
                                  void* data) {
    return SystemDesc{.mName       = name,
                      .mPhase      = phase,
                      .mRead       = read,
                      .mWrite      = write,
                      .mExclude    = ComponentMask{},
                      .mOptional   = ComponentMask{},
                      .mSends      = sends,
                      .mReceives   = receives,
                      .mMainThread = false,
                      .mFunction   = function,
                      .mData       = data};
}

} // namespace

TEST_CASE("message types register like components", "[ecs][messages]") {
    Fixture f;
    CHECK(f.mDamage.mValue == 0);
    CHECK(f.mPing.mValue == 1);
    CHECK(f.mWorld.GetMessages().Find<Damage>() == f.mDamage);
    CHECK(*f.mWorld.GetMessages().Register<Damage>() == f.mDamage);
    CHECK(f.mWorld.GetMessages().GetInfo(f.mPing).mSize == sizeof(Ping));
    CHECK_FALSE(f.mWorld.GetMessages().Find(oyi::core::MakeTypeId("nothing")).IsValid());
}

TEST_CASE("an outbox and an inbox move typed payloads", "[ecs][messages]") {
    SystemAllocator allocator;
    MessageBuffer outbox(allocator);
    MessageInbox inbox(allocator);
    const MessageTypeIndex damage{.mValue = 3};
    const MessageTypeIndex ping{.mValue = 0};
    const EntityId target{.mIndex = 7, .mGeneration = 1};

    outbox.Send(damage, target, Damage{.mSource = EntityId::Invalid(), .mAmount = 2.5f});
    outbox.Send(ping, EntityId::Invalid(), Ping{.mSerial = 9});
    REQUIRE(outbox.GetRecords().size() == 2);
    CHECK(outbox.GetRecords()[0].mPayloadSize == sizeof(Damage));

    for (const MessageBuffer::Record& record : outbox.GetRecords()) {
        inbox.Push(record.mType, record.mTarget, outbox.GetPayload().data() + record.mPayloadOffset,
                   record.mPayloadSize);
    }
    CHECK(inbox.Count(damage) == 1);
    CHECK(inbox.Count(ping) == 1);
    CHECK(inbox.Count(MessageTypeIndex{.mValue = 5}) == 0);
    CHECK(inbox.Targets(damage)[0] == target);
    CHECK(inbox.Payloads<Damage>(damage)[0].mAmount == 2.5f);
    CHECK_FALSE(inbox.Targets(ping)[0].IsValid());
    CHECK(inbox.Payloads<Ping>(ping)[0].mSerial == 9);

    inbox.Clear();
    outbox.Clear();
    CHECK(inbox.IsEmpty());
    CHECK(outbox.IsEmpty());
}

TEST_CASE("messages reach the receivers whose query matches the target, one phase later", "[ecs][messages]") {
    Fixture f;
    const Types types{.mPosition = f.mPosition, .mHealth = f.mHealth, .mDamage = f.mDamage, .mPing = f.mPing};
    Attacker attacker{.mTypes = types};
    Defender defender;
    defender.mTypes = types;
    // Two attackable entities and one with only a Position: Damage to it has no matching receiver.
    const EntityId a     = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mHealth}));
    const EntityId b     = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mHealth}));
    const EntityId ghost = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    (void)ghost;

    REQUIRE(f.mSchedule
                .AddSystem(Describe("attacker", Phase::FixedSimulation, MaskOf({f.mPosition}), {},
                                    MessagesOf({f.mDamage, f.mPing}), {}, &Attacker::Run, &attacker))
                .has_value());
    REQUIRE(f.mSchedule
                .AddSystem(Describe("defender", Phase::Variable, {}, MaskOf({f.mHealth}), {},
                                    MessagesOf({f.mDamage, f.mPing}), &Defender::Run, &defender))
                .has_value());
    REQUIRE(f.mSchedule.Build().has_value());

    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(defender.mDamageSeen == 2); // the ghost has no Health, so the defender's query rejects it
    CHECK(defender.mPingsSeen == 1);  // broadcast
    CHECK(defender.mLastSerial == 1);
    CHECK(f.mWorld.Get<Health>(a)->mCurrent == 99);
    CHECK(f.mWorld.Get<Health>(b)->mCurrent == 99);

    // Nothing new was sent: the inbox was cleared after the run.
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(defender.mDamageSeen == 2);
    CHECK(defender.mPingsSeen == 1);

    // A target that dies before the sync point is dropped.
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
    REQUIRE(f.mWorld.DestroyEntity(a).has_value());
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(defender.mDamageSeen == 4); // the messages were routed at the end of FixedSimulation, before the destroy
    CHECK(f.mWorld.Get<Health>(b)->mCurrent == 98);
}

TEST_CASE("the context reports what a system may send and receive", "[ecs][messages]") {
    Fixture f;
    struct Probe {
        Types mTypes;
        bool mCanSendDamage    = false;
        bool mCanSendPing      = false;
        bool mCanReceivePing   = false;
        bool mCanReceiveDamage = false;
    };
    Probe probe{.mTypes =
                    Types{.mPosition = f.mPosition, .mHealth = f.mHealth, .mDamage = f.mDamage, .mPing = f.mPing}};
    const auto probeSystem = [](SystemContext& context) {
        auto* const self        = context.GetData<Probe>();
        self->mCanSendDamage    = context.CanSend(self->mTypes.mDamage);
        self->mCanSendPing      = context.CanSend(self->mTypes.mPing);
        self->mCanReceivePing   = context.CanReceive(self->mTypes.mPing);
        self->mCanReceiveDamage = context.CanReceive(self->mTypes.mDamage);
    };
    REQUIRE(f.mSchedule
                .AddSystem(Describe("probe", Phase::Input, {}, {}, MessagesOf({f.mDamage}), MessagesOf({f.mPing}),
                                    probeSystem, &probe))
                .has_value());
    REQUIRE(f.mSchedule.Build().has_value());
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Input).has_value());
    CHECK(probe.mCanSendDamage);
    CHECK_FALSE(probe.mCanSendPing);
    CHECK(probe.mCanReceivePing);
    CHECK_FALSE(probe.mCanReceiveDamage);
}

TEST_CASE("parallel senders deliver in the same order as serial ones", "[ecs][messages][job]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        const Types types{.mPosition = f.mPosition, .mHealth = f.mHealth, .mDamage = f.mDamage, .mPing = f.mPing};
        for (uint32_t i = 0; i < 200; ++i) {
            (void)*f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mHealth}));
        }
        // Four attackers run side by side (read-only, no conflicts); one defender collects.
        std::vector<Attacker> attackers(4, Attacker{.mTypes = types});
        Defender defender;
        defender.mTypes = types;
        for (Attacker& attacker : attackers) {
            REQUIRE(f.mSchedule
                        .AddSystem(Describe("attacker", Phase::FixedSimulation, MaskOf({f.mPosition}), {},
                                            MessagesOf({f.mDamage, f.mPing}), {}, &Attacker::Run, &attacker))
                        .has_value());
        }
        REQUIRE(f.mSchedule
                    .AddSystem(Describe("defender", Phase::Variable, {}, MaskOf({f.mHealth}), {},
                                        MessagesOf({f.mDamage, f.mPing}), &Defender::Run, &defender))
                    .has_value());
        REQUIRE(f.mSchedule.Build().has_value());
        if (parallel) {
            JobSystem jobs(
                f.mAllocator,
                JobSystem::Config{.mWorkerCount = 3, .mMaxJobs = 256, .mMaxCounters = 64, .mSpinIterations = 8});
            REQUIRE(f.mSchedule.RunFrame(jobs).has_value());
        } else {
            REQUIRE(f.mSchedule.RunFrameSerial().has_value());
        }
        return std::pair{defender.mPingsSeen, defender.mAmounts};
    };
    const auto serial   = simulate(false);
    const auto parallel = simulate(true);
    CHECK(serial.first == 4);
    CHECK(serial.second.size() == size_t{4} * 200);
    CHECK(serial.second == parallel.second);
}
