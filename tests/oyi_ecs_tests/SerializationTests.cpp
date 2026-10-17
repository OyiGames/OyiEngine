#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs/WorldFile.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <ios>
#include <span>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::EntityMark;
using oyi::ecs::EntityRegistry;
using oyi::ecs::World;
using namespace oyi::ecs_fixture;

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

struct Types {
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mVelocity;
    ComponentTypeIndex mHealth;
    ComponentTypeIndex mFrozen;
    ComponentTypeIndex mBounds;
};

[[nodiscard]] Types RegisterAll(World& world) {
    return Types{.mPosition = *world.GetComponents().Register<Position>(),
                 .mVelocity = *world.GetComponents().Register<Velocity>(),
                 .mHealth   = *world.GetComponents().Register<Health>(),
                 .mFrozen   = *world.GetComponents().Register<Frozen>(),
                 .mBounds   = *world.GetComponents().Register<ChunkBounds>()};
}

/// Three archetypes, holes in the entity table, recycled generations, enable bits, a chunk
/// component, an advanced world version: everything the format has to carry.
struct Populated {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    Types mTypes = RegisterAll(mWorld);
    std::vector<EntityId> mAlive;
    EntityId mDisabled;
    EntityId mRecycled;

    Populated() {
        std::vector<EntityId> doomed;
        for (uint32_t i = 0; i < 1500; ++i) {
            ComponentMask mask = MaskOf({mTypes.mPosition});
            if (i % 3 == 1) {
                mask = MaskOf({mTypes.mPosition, mTypes.mVelocity, mTypes.mHealth});
            } else if (i % 3 == 2) {
                mask = MaskOf({mTypes.mPosition, mTypes.mFrozen, mTypes.mBounds});
            }
            const EntityId entity                    = *mWorld.CreateEntity(mask);
            mWorld.GetForWrite<Position>(entity)->mX = static_cast<float>(i);
            if (mask.Test(mTypes.mHealth)) {
                mWorld.GetForWrite<Health>(entity)->mCurrent = static_cast<uint16_t>(i % 100);
            }
            if (i % 7 == 0) {
                doomed.push_back(entity);
            } else {
                mAlive.push_back(entity);
            }
        }
        for (const EntityId entity : doomed) {
            REQUIRE(mWorld.DestroyEntity(entity).has_value());
        }
        // One index comes back with a higher generation.
        mRecycled = *mWorld.CreateEntity(MaskOf({mTypes.mPosition}));
        CHECK(mRecycled.mGeneration == 2);
        mAlive.push_back(mRecycled);
        mDisabled = mAlive[5];
        mWorld.SetComponentEnabled(mDisabled, mTypes.mPosition, false);
        static_cast<ChunkBounds*>(mWorld.GetChunkComponentForWrite(mAlive[1], mTypes.mBounds))->mMax[0] = 9.0f;
        mWorld.AdvanceVersion(41);
    }
};

[[nodiscard]] std::pmr::vector<std::byte> SaveOf(const World& world) {
    std::pmr::vector<std::byte> bytes;
    REQUIRE(world.Save(bytes).has_value());
    return bytes;
}

} // namespace

TEST_CASE("a saved world loads back byte for byte with its entity handles", "[ecs][serialization]") {
    Populated original;
    const std::pmr::vector<std::byte> bytes = SaveOf(original.mWorld);
    CHECK(bytes.size() > size_t{16} * 1024 * 4);

    SystemAllocator allocator;
    World loaded(allocator);
    const Types types = RegisterAll(loaded);
    REQUIRE(loaded.Load(bytes).has_value());
    CHECK(loaded.ComputeStateHash() == original.mWorld.ComputeStateHash());
    CHECK(loaded.GetEntityCount() == original.mWorld.GetEntityCount());
    CHECK(loaded.GetArchetypeCount() == original.mWorld.GetArchetypeCount());
    CHECK(loaded.GetVersion() == original.mWorld.GetVersion());
    for (const EntityId entity : original.mAlive) {
        REQUIRE(loaded.IsAlive(entity));
        CHECK(loaded.Get<Position>(entity)->mX == original.mWorld.Get<Position>(entity)->mX);
        CHECK(*loaded.GetComponentMask(entity) == *original.mWorld.GetComponentMask(entity));
    }
    CHECK(loaded.Get<Health>(original.mAlive[0])->mCurrent ==
          original.mWorld.Get<Health>(original.mAlive[0])->mCurrent);
    CHECK_FALSE(loaded.IsComponentEnabled(original.mDisabled, types.mPosition));
    CHECK(static_cast<const ChunkBounds*>(loaded.GetChunkComponent(original.mAlive[1], types.mBounds))->mMax[0] ==
          9.0f);
    CHECK(loaded.IsAlive(original.mRecycled));
    // Saving the loaded world reproduces the file.
    CHECK(SaveOf(loaded) == bytes);
    // Dead indices are recycled lowest first, with their next generation (index 0 died in the
    // fixture and the original's free list handed out the highest index first).
    const EntityId reborn = *loaded.CreateEntity(MaskOf({types.mPosition}));
    CHECK(reborn.mIndex == 0);
    CHECK(reborn.mGeneration == 2);
}

TEST_CASE("a saved world describes itself without a registry", "[ecs][serialization]") {
    Populated original;
    const std::pmr::vector<std::byte> bytes = SaveOf(original.mWorld);
    oyi::ecs::WorldFileSummary summary(original.mAllocator);
    REQUIRE(oyi::ecs::DescribeSavedWorld(bytes, summary).has_value());
    CHECK(summary.mFormatVersion == 2);
    CHECK(summary.mSchemaHash == original.mWorld.ComputeSchemaHash());
    CHECK(summary.mChunkSize == 16 * 1024);
    CHECK(summary.mWorldVersion == original.mWorld.GetVersion());
    CHECK(summary.mEntityCount == original.mWorld.GetEntityCount());
    CHECK(summary.mEntityCapacity == 1500);
    REQUIRE(summary.mComponents.size() == 5);
    CHECK(summary.mComponents[0].mName == "oyi::ecs_fixture::Position");
    CHECK(summary.mComponents[0].mSize == sizeof(Position));
    REQUIRE(summary.mArchetypes.size() == 3);
    uint32_t counted = 0;
    for (const auto& archetype : summary.mArchetypes) {
        CHECK(archetype.mChunkCount >= 1);
        CHECK(archetype.mTypes[0] == 0);
        counted += archetype.mEntityCount;
    }
    CHECK(counted == summary.mEntityCount);
    // Damage is reported, not crashed on.
    for (const size_t cut : {size_t{2}, size_t{30}, bytes.size() / 3, bytes.size() - 1}) {
        CHECK(oyi::ecs::DescribeSavedWorld(std::span<const std::byte>(bytes.data(), cut), summary).error().mCode ==
              ErrorCode::CorruptData);
    }
    // Left behind for the command-line test (oyi world inspect).
    std::ofstream file(OYI_TEST_OUTPUT_DIR "/oyi_cli_world.oyiw", std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size())); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    CHECK(file.good());
}

TEST_CASE("an empty world round-trips and a loaded world can be cleared and loaded again", "[ecs][serialization]") {
    SystemAllocator allocator;
    World world(allocator);
    (void)RegisterAll(world);
    const std::pmr::vector<std::byte> empty = SaveOf(world);
    World other(allocator);
    (void)RegisterAll(other);
    REQUIRE(other.Load(empty).has_value());
    CHECK(other.GetEntityCount() == 0);

    const Populated populated;
    const std::pmr::vector<std::byte> bytes = SaveOf(populated.mWorld);
    // A world that only ever loaded an empty file is still empty, so it accepts another file.
    REQUIRE(other.Load(bytes).has_value());
    CHECK(other.GetEntityCount() == populated.mWorld.GetEntityCount());
    World fresh(allocator);
    (void)RegisterAll(fresh);
    REQUIRE(fresh.Load(bytes).has_value());
    CHECK(fresh.GetEntityCount() == populated.mWorld.GetEntityCount());
    fresh.Clear();
    CHECK(fresh.GetEntityCount() == 0);
    CHECK(fresh.GetArchetypeCount() == 0);
    REQUIRE(fresh.Load(bytes).has_value());
    CHECK(fresh.ComputeStateHash() == populated.mWorld.ComputeStateHash());
}

TEST_CASE("loading refuses the wrong world, the wrong schema and damaged files", "[ecs][serialization]") {
    Populated populated;
    const std::pmr::vector<std::byte> bytes = SaveOf(populated.mWorld);
    SystemAllocator allocator;

    SECTION("a world that already has entities") {
        CHECK(populated.mWorld.Load(bytes).error().mCode == ErrorCode::InvalidState);
    }
    SECTION("a registry with the types in another order") {
        World other(allocator);
        (void)*other.GetComponents().Register<Velocity>();
        (void)*other.GetComponents().Register<Position>();
        (void)*other.GetComponents().Register<Health>();
        (void)*other.GetComponents().Register<Frozen>();
        (void)*other.GetComponents().Register<ChunkBounds>();
        CHECK(other.ComputeSchemaHash() != populated.mWorld.ComputeSchemaHash());
        CHECK(other.Load(bytes).error().mCode == ErrorCode::CorruptData);
        CHECK(other.GetEntityCount() == 0);
    }
    SECTION("a registry missing a type") {
        World other(allocator);
        (void)*other.GetComponents().Register<Position>();
        CHECK(other.Load(bytes).error().mCode == ErrorCode::CorruptData);
    }
    SECTION("another chunk size") {
        World other(allocator, World::Config{.mChunkSize = 32 * 1024, .mKind = oyi::ecs::WorldKind::Client});
        (void)RegisterAll(other);
        CHECK(other.Load(bytes).error().mCode == ErrorCode::CorruptData);
    }
    SECTION("truncated anywhere") {
        World other(allocator);
        (void)RegisterAll(other);
        for (const size_t cut : {size_t{0}, size_t{3}, size_t{12}, size_t{40}, bytes.size() / 2, bytes.size() - 1}) {
            const auto failed = other.Load(std::span<const std::byte>(bytes.data(), cut));
            REQUIRE_FALSE(failed.has_value());
            CHECK(failed.error().mCode == ErrorCode::CorruptData);
            CHECK(other.GetEntityCount() == 0);
            CHECK(other.GetArchetypeCount() == 0);
        }
    }
    SECTION("a wrong magic and a trailing byte") {
        World other(allocator);
        (void)RegisterAll(other);
        std::pmr::vector<std::byte> damaged = bytes;
        damaged[0]                          = std::byte{'X'};
        CHECK(other.Load(damaged).error().mCode == ErrorCode::CorruptData);
        std::pmr::vector<std::byte> longer = bytes;
        longer.push_back(std::byte{0});
        CHECK(other.Load(longer).error().mCode == ErrorCode::CorruptData);
        CHECK(other.GetEntityCount() == 0);
    }
}

TEST_CASE("transient components are saved as their defaults", "[ecs][serialization]") {
    SystemAllocator allocator;
    World world(allocator);
    const ComponentTypeIndex position          = *world.GetComponents().Register<Position>();
    const ComponentTypeIndex scratch           = *world.GetComponents().Register<Scratch>();
    const EntityId entity                      = *world.CreateEntity(MaskOf({position, scratch}));
    world.GetForWrite<Position>(entity)->mX    = 3.0f;
    world.GetForWrite<Scratch>(entity)->mValue = 5.0f;
    const std::pmr::vector<std::byte> bytes    = SaveOf(world);
    CHECK(world.Get<Scratch>(entity)->mValue == 5.0f); // saving does not touch the live world

    World loaded(allocator);
    REQUIRE(loaded.GetComponents().Register<Position>().has_value());
    REQUIRE(loaded.GetComponents().Register<Scratch>().has_value());
    REQUIRE(loaded.Load(bytes).has_value());
    CHECK(loaded.Get<Position>(entity)->mX == 3.0f);
    CHECK(loaded.Get<Scratch>(entity)->mValue == 0.0f);
}

TEST_CASE("the schema hash follows the registered types", "[ecs][serialization]") {
    SystemAllocator allocator;
    World a(allocator);
    World b(allocator);
    CHECK(a.ComputeSchemaHash() == b.ComputeSchemaHash());
    (void)RegisterAll(a);
    CHECK(a.ComputeSchemaHash() != b.ComputeSchemaHash());
    (void)RegisterAll(b);
    CHECK(a.ComputeSchemaHash() == b.ComputeSchemaHash());
}

TEST_CASE("a cloned entity copies every component and enable bit of its prototype", "[ecs][serialization][prototype]") {
    SystemAllocator allocator;
    World world(allocator);
    const Types types        = RegisterAll(world);
    const EntityId prototype = *world.CreateEntity(MaskOf({types.mPosition, types.mVelocity, types.mHealth}));
    world.GetForWrite<Position>(prototype)->mX     = 3.0f;
    world.GetForWrite<Velocity>(prototype)->mY     = -2.0f;
    world.GetForWrite<Health>(prototype)->mCurrent = 7;
    world.SetComponentEnabled(prototype, types.mVelocity, false);

    const EntityId clone = *world.CloneEntity(prototype);
    CHECK(clone != prototype);
    CHECK(*world.GetComponentMask(clone) == *world.GetComponentMask(prototype));
    CHECK(world.Get<Position>(clone)->mX == 3.0f);
    CHECK(world.Get<Velocity>(clone)->mY == -2.0f);
    CHECK(world.Get<Health>(clone)->mCurrent == 7);
    CHECK_FALSE(world.IsComponentEnabled(clone, types.mVelocity));
    CHECK(world.IsComponentEnabled(clone, types.mPosition));
    // The clone is its own entity.
    world.GetForWrite<Position>(clone)->mX = 4.0f;
    CHECK(world.Get<Position>(prototype)->mX == 3.0f);
    // Many clones cross chunk boundaries without disturbing the prototype.
    for (uint32_t i = 0; i < 2000; ++i) {
        REQUIRE(world.CloneEntity(prototype).has_value());
    }
    CHECK(world.Get<Position>(prototype)->mX == 3.0f);
    CHECK(world.GetEntityCount() == 2002);
    REQUIRE(world.DestroyEntity(prototype).has_value());
    CHECK(world.CloneEntity(prototype).error().mCode == ErrorCode::NotFound);
}

TEST_CASE("the entity registry restores its table with the lowest dead index on top", "[ecs][serialization]") {
    SystemAllocator allocator;
    EntityRegistry registry(allocator);
    const std::vector<uint32_t> generations{3, 1, 5, 2};
    const std::vector<uint8_t> alive{1, 0, 1, 0};
    registry.Restore(generations, alive);
    CHECK(registry.GetCapacity() == 4);
    CHECK(registry.GetAliveCount() == 2);
    CHECK(registry.IsAlive(EntityId{.mIndex = 0, .mGeneration = 3}));
    CHECK_FALSE(registry.IsAlive(EntityId{.mIndex = 1, .mGeneration = 1}));
    CHECK(registry.GetGeneration(3) == 2);
    CHECK(registry.GetGeneration(9) == 0);
    const EntityId first = registry.Create();
    CHECK(first.mIndex == 1);
    CHECK(first.mGeneration == 1);
    const EntityId second = registry.Create();
    CHECK(second.mIndex == 3);
    registry.Clear();
    CHECK(registry.GetCapacity() == 0);
    CHECK(registry.GetAliveCount() == 0);
}

TEST_CASE("marks and prototype archetypes survive the world file", "[ecs][serialization][object]") {
    SystemAllocator allocator;
    World world(allocator);
    const Types types                          = RegisterAll(world);
    const EntityId prototype                   = *world.CreatePrototype(MaskOf({types.mPosition, types.mVelocity}));
    world.GetForWrite<Position>(prototype)->mX = 9.0f;
    const EntityId locked                      = *world.CloneEntity(prototype);
    const EntityId plain                       = *world.CreateEntity(MaskOf({types.mPosition, types.mVelocity}));
    const EntityId unlocked                    = *world.CloneEntity(plain);
    REQUIRE(world.SetMark(plain, EntityMark::Locked).has_value());
    REQUIRE(world.SetMark(unlocked, EntityMark::None).has_value());
    const std::pmr::vector<std::byte> bytes = SaveOf(world);

    oyi::ecs::WorldFileSummary summary(allocator);
    REQUIRE(oyi::ecs::DescribeSavedWorld(bytes, summary).has_value());
    CHECK(summary.mEntityCount == 4);
    CHECK(summary.mPrototypeCount == 1);
    CHECK(summary.mLockedCount == 2);
    REQUIRE(summary.mArchetypes.size() == 2);
    CHECK((summary.mArchetypes[0].mPrototype ? 1 : 0) + (summary.mArchetypes[1].mPrototype ? 1 : 0) == 1);

    World loaded(allocator);
    const Types loadedTypes = RegisterAll(loaded);
    REQUIRE(loaded.Load(bytes).has_value());
    CHECK(loaded.GetMark(prototype) == EntityMark::Prototype);
    CHECK(loaded.GetMark(locked) == EntityMark::Locked);
    CHECK(loaded.GetMark(plain) == EntityMark::Locked);
    CHECK(loaded.GetMark(unlocked) == EntityMark::None);
    CHECK(loaded.Get<Position>(prototype)->mX == 9.0f);
    uint32_t rows = 0;
    loaded.ForEachChunk(MaskOf({loadedTypes.mPosition}), ComponentMask{},
                        [&rows](oyi::ecs::ChunkView& view) { rows += view.GetCount(); });
    CHECK(rows == 3); // the prototype stays out of iteration after loading too
    CHECK(loaded.AddComponent(locked, loadedTypes.mHealth).error().mCode == ErrorCode::InvalidState);
    const EntityId again = *loaded.CloneEntity(prototype);
    CHECK(loaded.GetMark(again) == EntityMark::Locked);
    CHECK(loaded.Get<Position>(again)->mX == 9.0f);
}
