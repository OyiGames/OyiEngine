#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_net/NetworkId.h>
#include <oyi_net/Replication.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>

namespace {

using Catch::Approx;
using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::World;
using oyi::net::ApplyDelta;
using oyi::net::ApplyEntity;
using oyi::net::DecodeComponent;
using oyi::net::EncodeComponent;
using oyi::net::EncodeDelta;
using oyi::net::EncodeEntity;
using oyi::net::NetworkId;
using oyi::net::NetworkIdMap;
using oyi::net::ReplicationSchema;
using namespace oyi::ecs_fixture;

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mNetPose  = *mWorld.GetComponents().Register<NetPose>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    ReplicationSchema mSchema{mAllocator};

    Fixture() { REQUIRE(mSchema.Build(mWorld.GetComponents()).has_value()); }
};

} // namespace

TEST_CASE("the replication schema picks the replicated fields and their wire size", "[net][replication]") {
    const Fixture f;
    CHECK(f.mSchema.GetReplicatedTypeCount() == 1);
    CHECK(f.mSchema.IsReplicated(f.mNetPose));
    CHECK_FALSE(f.mSchema.IsReplicated(f.mPosition));
    CHECK_FALSE(f.mSchema.IsReplicated(ComponentTypeIndex::Invalid()));
    CHECK(HasFlag(kNetPoseInfo.mFlags, oyi::core::ComponentFlags::Replicated));
    CHECK(HasFlag(kNetPoseInfo.mFlags, oyi::core::ComponentFlags::Predicted));
    const auto fields = f.mSchema.GetFields(f.mNetPose);
    REQUIRE(fields.size() == 3);
    CHECK(fields[0].mQuantize == 0.01f);
    CHECK(fields[1].mQuantize == 0.01f);
    CHECK(fields[2].mQuantize == 0.0f);
    CHECK(f.mSchema.GetEncodedSize(f.mNetPose) == 4 + 4 + 2); // two quantized floats as int32, one uint16
    CHECK(f.mSchema.GetEncodedSize(f.mPosition) == 0);
    CHECK(f.mSchema.GetFields(f.mPosition).empty());
}

TEST_CASE("a component encodes and decodes its replicated fields, quantized to the step", "[net][replication]") {
    const Fixture f;
    const NetPose source{.mX = 1.2345f, .mY = -7.891f, .mLocalOnly = 99.0f, .mHealth = 42, .mPad = {}};
    std::array<std::byte, 32> wire{};
    const auto size = EncodeComponent(f.mSchema, f.mNetPose, &source, wire);
    REQUIRE(size.has_value());
    CHECK(*size == 10);
    NetPose target{};
    target.mLocalOnly = 5.0f;
    REQUIRE(
        DecodeComponent(f.mSchema, f.mNetPose, std::span<const std::byte>(wire.data(), *size), &target).has_value());
    CHECK(target.mX == Approx(1.23f).margin(0.0051));
    CHECK(target.mY == Approx(-7.89f).margin(0.0051));
    CHECK(target.mHealth == 42);
    CHECK(target.mLocalOnly == 5.0f); // not replicated: untouched
    CHECK(EncodeComponent(f.mSchema, f.mPosition, &source, wire).error().mCode == ErrorCode::InvalidArgument);
    CHECK(EncodeComponent(f.mSchema, f.mNetPose, &source, std::span<std::byte>(wire.data(), 4)).error().mCode ==
          ErrorCode::OutOfRange);
    CHECK(DecodeComponent(f.mSchema, f.mNetPose, std::span<const std::byte>(wire.data(), 4), &target).error().mCode ==
          ErrorCode::CorruptData);
}

TEST_CASE("a delta carries only the fields whose wire form changed", "[net][replication]") {
    const Fixture f;
    const NetPose baseline{.mX = 1.0f, .mY = 2.0f, .mLocalOnly = 0.0f, .mHealth = 10, .mPad = {}};
    NetPose current = baseline;
    std::array<std::byte, 32> wire{};
    const auto nothing = EncodeDelta(f.mSchema, f.mNetPose, &baseline, &current, wire);
    REQUIRE(nothing.has_value());
    CHECK(*nothing == 8);
    current.mX += 0.001f; // below the quantization step: still nothing to send
    CHECK(*EncodeDelta(f.mSchema, f.mNetPose, &baseline, &current, wire) == 8);
    current.mX      = 3.5f;
    current.mHealth = 11;
    const auto two  = EncodeDelta(f.mSchema, f.mNetPose, &baseline, &current, wire);
    REQUIRE(two.has_value());
    CHECK(*two == 8 + 4 + 2);
    uint64_t mask = 0;
    std::memcpy(&mask, wire.data(), sizeof(mask));
    CHECK(mask == 0b101);
    NetPose applied = baseline;
    REQUIRE(ApplyDelta(f.mSchema, f.mNetPose, std::span<const std::byte>(wire.data(), *two), &applied).has_value());
    CHECK(applied.mX == Approx(3.5f).margin(0.0051));
    CHECK(applied.mY == 2.0f);
    CHECK(applied.mHealth == 11);
    CHECK(ApplyDelta(f.mSchema, f.mNetPose, std::span<const std::byte>(wire.data(), 9), &applied).error().mCode ==
          ErrorCode::CorruptData);
    CHECK(EncodeDelta(f.mSchema, f.mNetPose, &baseline, &current, std::span<std::byte>(wire.data(), 9)).error().mCode ==
          ErrorCode::OutOfRange);
}

TEST_CASE("an entity record crosses from an authority to a replica through network ids", "[net][replication]") {
    Fixture server;
    Fixture client;
    const EntityId authority = *server.mWorld.CreateEntity(MaskOf({server.mPosition, server.mNetPose, server.mHealth}));
    server.mWorld.GetForWrite<NetPose>(authority)->mX      = 10.5f;
    server.mWorld.GetForWrite<NetPose>(authority)->mHealth = 77;
    server.mWorld.GetForWrite<Position>(authority)->mX     = 1.0f; // not replicated
    NetworkIdMap serverIds(server.mAllocator);
    const NetworkId id = *serverIds.Assign(authority);
    CHECK(id.mValue == 1);
    CHECK(serverIds.Assign(authority).error().mCode == ErrorCode::AlreadyExists);

    std::array<std::byte, 64> wire{};
    const auto size = EncodeEntity(server.mSchema, server.mWorld, authority, wire);
    REQUIRE(size.has_value());
    CHECK(*size == 2 + 10 + 2);

    const EntityId replica = *client.mWorld.CreateEntity(MaskOf({client.mPosition, client.mNetPose}));
    NetworkIdMap clientIds(client.mAllocator);
    REQUIRE(clientIds.Bind(id, replica).has_value());
    CHECK(clientIds.Resolve(id) == replica);
    CHECK(clientIds.Find(replica) == id);
    CHECK(clientIds.Bind(id, replica).error().mCode == ErrorCode::AlreadyExists);
    CHECK(clientIds.Bind(NetworkId{}, replica).error().mCode == ErrorCode::InvalidArgument);
    REQUIRE(ApplyEntity(client.mSchema, client.mWorld, clientIds.Resolve(id),
                        std::span<const std::byte>(wire.data(), *size))
                .has_value());
    CHECK(client.mWorld.Get<NetPose>(replica)->mX == Approx(10.5f).margin(0.0051));
    CHECK(client.mWorld.Get<NetPose>(replica)->mHealth == 77);
    CHECK(client.mWorld.Get<Position>(replica)->mX == 0.0f);

    // A replica without the type, a dead entity, a truncated record.
    const EntityId bare = *client.mWorld.CreateEntity(MaskOf({client.mPosition}));
    CHECK(ApplyEntity(client.mSchema, client.mWorld, bare, std::span<const std::byte>(wire.data(), *size))
              .error()
              .mCode == ErrorCode::NotFound);
    CHECK(EncodeEntity(server.mSchema, server.mWorld, EntityId::Invalid(), wire).error().mCode == ErrorCode::NotFound);
    CHECK(
        ApplyEntity(client.mSchema, client.mWorld, replica, std::span<const std::byte>(wire.data(), 5)).error().mCode ==
        ErrorCode::CorruptData);
    CHECK(EncodeEntity(server.mSchema, server.mWorld, authority, std::span<std::byte>(wire.data(), 6)).error().mCode ==
          ErrorCode::OutOfRange);
    // An entity with nothing replicated is an empty record.
    const EntityId plain = *server.mWorld.CreateEntity(MaskOf({server.mPosition}));
    CHECK(*EncodeEntity(server.mSchema, server.mWorld, plain, wire) == 2);

    serverIds.Release(id);
    CHECK_FALSE(serverIds.Resolve(id).IsValid());
    CHECK(serverIds.GetCount() == 0);
    const NetworkId next = *serverIds.Assign(plain);
    CHECK(next.mValue == 2);                   // ids are never reused
    clientIds.Release(NetworkId{.mValue = 9}); // unknown: harmless
    CHECK(clientIds.GetCount() == 1);
}
