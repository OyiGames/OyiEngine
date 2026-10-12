// Several worlds in one process (ADR-0017): an authoritative server and thin clients that only
// hold replicas, plus one predicting client that runs ahead, takes corrections for an older tick,
// rolls back and resimulates with the inputs it kept. Everything goes through the hooks the
// kernel offers; there is no transport, the "packets" are byte vectors handed over directly.
#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_net/NetworkId.h>
#include <oyi_net/Prediction.h>
#include <oyi_net/Replication.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

using Catch::Approx;
using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::World;
using oyi::ecs::WorldKind;
using oyi::net::ApplyDelta;
using oyi::net::EncodeDelta;
using oyi::net::InputHistory;
using oyi::net::NetworkId;
using oyi::net::NetworkIdMap;
using oyi::net::PredictionHistory;
using oyi::net::ReplicationSchema;
using namespace oyi::ecs_fixture;

constexpr uint32_t kActors   = 50;
constexpr uint32_t kClients  = 3;
constexpr uint32_t kTicks    = 40;
constexpr uint32_t kHistory  = 16;
constexpr uint32_t kWireSize = 32;

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

struct Input {
    float mDx = 0.0f;
};

/// The one rule of this little game: a NetPose moves by its input on x and drifts on y.
void Simulate(World& world, EntityId entity, const Input& input) {
    auto* const pose = world.GetForWrite<NetPose>(entity);
    pose->mX += input.mDx;
    pose->mY += 0.25f;
}

[[nodiscard]] Input InputFor(uint32_t actor, uint64_t tick) {
    return Input{.mDx = static_cast<float>((actor + tick) % 3) - 1.0f};
}

struct Peer {
    SystemAllocator mAllocator;
    World mWorld;
    ComponentTypeIndex mNetPose;
    ComponentTypeIndex mPosition;
    ReplicationSchema mSchema{mAllocator};
    NetworkIdMap mIds{mAllocator};
    std::vector<EntityId> mActors;
    std::vector<NetPose> mBaselines; ///< the last state each side agreed on, per actor

    explicit Peer(WorldKind kind)
        : mWorld(mAllocator, World::Config{.mChunkSize = 16 * 1024, .mKind = kind})
        , mNetPose(*mWorld.GetComponents().Register<NetPose>())
        , mPosition(*mWorld.GetComponents().Register<Position>()) {
        REQUIRE(mSchema.Build(mWorld.GetComponents()).has_value());
        mBaselines.resize(kActors);
    }
};

} // namespace

TEST_CASE("thin clients converge on the authoritative state through deltas", "[net][thinclient]") {
    Peer server(WorldKind::Server);
    std::array<Peer*, kClients> clients{};
    Peer clientA(WorldKind::Client);
    Peer clientB(WorldKind::Client);
    Peer clientC(WorldKind::Client);
    clients = {&clientA, &clientB, &clientC};

    // Spawn on the server, replicate the ids, spawn replicas.
    for (uint32_t actor = 0; actor < kActors; ++actor) {
        const EntityId authority = *server.mWorld.CreateEntity(MaskOf({server.mNetPose, server.mPosition}));
        server.mActors.push_back(authority);
        const NetworkId id = *server.mIds.Assign(authority);
        for (Peer* const client : clients) {
            const EntityId replica = *client->mWorld.CreateEntity(MaskOf({client->mNetPose}));
            REQUIRE(client->mIds.Bind(id, replica).has_value());
            client->mActors.push_back(replica);
        }
    }

    std::array<std::byte, kWireSize> wire{};
    size_t bytesOnTheWire = 0;
    for (uint64_t tick = 1; tick <= kTicks; ++tick) {
        for (uint32_t actor = 0; actor < kActors; ++actor) {
            Simulate(server.mWorld, server.mActors[actor], InputFor(actor, tick));
        }
        // Every other tick the server sends each client the delta against what it last sent.
        if (tick % 2 == 0) {
            for (uint32_t actor = 0; actor < kActors; ++actor) {
                const NetPose& current = *server.mWorld.Get<NetPose>(server.mActors[actor]);
                for (Peer* const client : clients) {
                    const auto size =
                        EncodeDelta(server.mSchema, server.mNetPose, &client->mBaselines[actor], &current, wire);
                    REQUIRE(size.has_value());
                    bytesOnTheWire += *size;
                    const EntityId replica = client->mIds.Resolve(server.mIds.Find(server.mActors[actor]));
                    REQUIRE(ApplyDelta(client->mSchema, client->mNetPose,
                                       std::span<const std::byte>(wire.data(), *size),
                                       client->mWorld.GetForWrite<NetPose>(replica))
                                .has_value());
                    client->mBaselines[actor] = current;
                }
            }
        }
    }
    for (uint32_t actor = 0; actor < kActors; ++actor) {
        const NetPose& truth = *server.mWorld.Get<NetPose>(server.mActors[actor]);
        for (Peer* const client : clients) {
            const NetPose& replica = *client->mWorld.Get<NetPose>(client->mActors[actor]);
            CHECK(replica.mX == Approx(truth.mX).margin(0.0051));
            CHECK(replica.mY == Approx(truth.mY).margin(0.0051));
            CHECK(replica.mHealth == truth.mHealth);
        }
    }
    // Deltas beat full state: the y drift and x walk change every tick, health never does.
    const size_t fullState =
        static_cast<size_t>(kTicks / 2) * kActors * kClients * server.mSchema.GetEncodedSize(server.mNetPose);
    CHECK(bytesOnTheWire > 0);
    CHECK(bytesOnTheWire < fullState + (static_cast<size_t>(kTicks / 2) * kActors * kClients * 8));
}

TEST_CASE("a predicting client rolls back to a correction and resimulates its kept inputs", "[net][thinclient]") {
    Peer server(WorldKind::Server);
    Peer client(WorldKind::Client);
    const EntityId authority = *server.mWorld.CreateEntity(MaskOf({server.mNetPose}));
    const EntityId avatar    = *client.mWorld.CreateEntity(MaskOf({client.mNetPose}));
    const NetworkId id       = *server.mIds.Assign(authority);
    REQUIRE(client.mIds.Bind(id, avatar).has_value());
    PredictionHistory history(client.mAllocator, kHistory);
    history.Build(client.mWorld.GetComponents());
    InputHistory<Input> inputs(client.mAllocator, kHistory);
    std::array<std::byte, kWireSize> wire{};

    // The client predicts ticks 1..10 with its own inputs; the server only knows ticks 1..6,
    // and for tick 4 it saw a different input (a dropped packet): the states diverge.
    for (uint64_t tick = 1; tick <= 10; ++tick) {
        const Input input = InputFor(0, tick);
        inputs.Push(tick, input);
        Simulate(client.mWorld, avatar, input);
        history.Record(client.mWorld, tick);
    }
    for (uint64_t tick = 1; tick <= 6; ++tick) {
        Simulate(server.mWorld, authority, tick == 4 ? Input{.mDx = 5.0f} : InputFor(0, tick));
    }
    const float predicted = client.mWorld.Get<NetPose>(avatar)->mX;
    CHECK(predicted != Approx(server.mWorld.Get<NetPose>(authority)->mX));

    // The correction for tick 6 arrives: roll back to 6, apply the authoritative state, replay 7..10.
    const NetPose& truthAtSix = *server.mWorld.Get<NetPose>(authority);
    NetPose baseline{};
    const auto size = EncodeDelta(server.mSchema, server.mNetPose, &baseline, &truthAtSix, wire);
    REQUIRE(size.has_value());
    REQUIRE(history.Rollback(client.mWorld, 6).has_value());
    REQUIRE(ApplyDelta(client.mSchema, client.mNetPose, std::span<const std::byte>(wire.data(), *size),
                       client.mWorld.GetForWrite<NetPose>(avatar))
                .has_value());
    history.Truncate(6);
    history.Record(client.mWorld, 6);
    for (uint64_t tick = 7; tick <= 10; ++tick) {
        const Input* const kept = inputs.Get(tick);
        REQUIRE(kept != nullptr);
        Simulate(client.mWorld, avatar, *kept);
        history.Record(client.mWorld, tick);
    }
    // The server, catching up with the same inputs, ends where the client already is.
    for (uint64_t tick = 7; tick <= 10; ++tick) {
        Simulate(server.mWorld, authority, InputFor(0, tick));
    }
    CHECK(client.mWorld.Get<NetPose>(avatar)->mX == Approx(server.mWorld.Get<NetPose>(authority)->mX).margin(0.0051));
    CHECK(client.mWorld.Get<NetPose>(avatar)->mY == Approx(server.mWorld.Get<NetPose>(authority)->mY).margin(0.0051));
    CHECK(history.GetNewestTick() == 10);
    CHECK(history.GetOldestTick() == 1); // Truncate only drops what was newer than the correction
    CHECK(client.mWorld.GetKind() == WorldKind::Client);
    CHECK(server.mWorld.GetKind() == WorldKind::Server);
}
