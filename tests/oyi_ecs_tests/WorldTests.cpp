#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <span>
#include <unordered_map>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::World;
using namespace oyi::ecs_fixture;

/// A world with every fixture type registered.
struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition  = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity  = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mHealth    = *mWorld.GetComponents().Register<Health>();
    ComponentTypeIndex mTransform = *mWorld.GetComponents().Register<Transform>();
    ComponentTypeIndex mFrozen    = *mWorld.GetComponents().Register<Frozen>();
    ComponentTypeIndex mBounds    = *mWorld.GetComponents().Register<ChunkBounds>();
};

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

} // namespace

TEST_CASE("entities are created with defaulted components and destroyed cleanly", "[ecs][world]") {
    Fixture f;
    const auto entity = f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mHealth}));
    REQUIRE(entity.has_value());
    CHECK(f.mWorld.IsAlive(*entity));
    CHECK(f.mWorld.GetEntityCount() == 1);
    CHECK(f.mWorld.HasComponent(*entity, f.mHealth));
    CHECK_FALSE(f.mWorld.HasComponent(*entity, f.mVelocity));

    const auto* const health = f.mWorld.Get<Health>(*entity);
    REQUIRE(health != nullptr);
    CHECK(health->mCurrent == 100);
    CHECK(health->mMax == 100);
    CHECK(f.mWorld.Get<Velocity>(*entity) == nullptr);

    auto* const position = f.mWorld.GetForWrite<Position>(*entity);
    REQUIRE(position != nullptr);
    position->mX = 3.0f;
    CHECK(f.mWorld.Get<Position>(*entity)->mX == 3.0f);

    REQUIRE(f.mWorld.DestroyEntity(*entity).has_value());
    CHECK_FALSE(f.mWorld.IsAlive(*entity));
    CHECK(f.mWorld.Get<Position>(*entity) == nullptr);
    CHECK(f.mWorld.DestroyEntity(*entity).error().mCode == ErrorCode::NotFound);
    CHECK(f.mWorld.GetStats().mChunks == 0);
}

TEST_CASE("unregistered types are rejected", "[ecs][world]") {
    Fixture f;
    const std::array<ComponentTypeIndex, 1> types{ComponentTypeIndex{.mValue = 77}};
    const auto entity = f.mWorld.CreateEntity(std::span<const ComponentTypeIndex>{types});
    REQUIRE_FALSE(entity.has_value());
    CHECK(entity.error().mCode == ErrorCode::InvalidArgument);
}

TEST_CASE("adding and removing components moves the entity and keeps its data", "[ecs][world]") {
    Fixture f;
    const EntityId entity                      = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    f.mWorld.GetForWrite<Position>(entity)->mY = 7.0f;

    REQUIRE(f.mWorld.AddComponent(entity, f.mVelocity).has_value());
    CHECK(f.mWorld.HasComponent(entity, f.mVelocity));
    CHECK(f.mWorld.Get<Position>(entity)->mY == 7.0f);
    CHECK(f.mWorld.Get<Velocity>(entity)->mX == 0.0f);
    CHECK(f.mWorld.AddComponent(entity, f.mVelocity).error().mCode == ErrorCode::AlreadyExists);
    CHECK(f.mWorld.GetArchetypeCount() == 2);

    REQUIRE(f.mWorld.RemoveComponent(entity, f.mPosition).has_value());
    CHECK_FALSE(f.mWorld.HasComponent(entity, f.mPosition));
    CHECK(f.mWorld.Get<Velocity>(entity) != nullptr);
    CHECK(f.mWorld.RemoveComponent(entity, f.mPosition).error().mCode == ErrorCode::NotFound);
    // The archetypes the entity left are empty, so they hold no chunks.
    CHECK(f.mWorld.GetStats().mChunks == 1);
}

TEST_CASE("removing a row swaps the last row in and the moved entity still resolves", "[ecs][world]") {
    Fixture f;
    const ComponentMask mask = MaskOf({f.mPosition});
    std::vector<EntityId> entities;
    entities.reserve(10);
    for (uint32_t i = 0; i < 10; ++i) {
        entities.push_back(*f.mWorld.CreateEntity(mask));
        f.mWorld.GetForWrite<Position>(entities.back())->mX = static_cast<float>(i);
    }
    REQUIRE(f.mWorld.DestroyEntity(entities[3]).has_value());
    for (uint32_t i = 0; i < 10; ++i) {
        if (i == 3) {
            continue;
        }
        REQUIRE(f.mWorld.IsAlive(entities[i]));
        CHECK(f.mWorld.Get<Position>(entities[i])->mX == static_cast<float>(i));
    }
    uint32_t rows = 0;
    f.mWorld.ForEachChunk(mask, ComponentMask{}, [&rows](ChunkView& view) { rows += view.GetCount(); });
    CHECK(rows == 9);
}

TEST_CASE("chunks fill up, spill over and are released when emptied", "[ecs][world]") {
    Fixture f;
    const ComponentMask mask = MaskOf({f.mTransform});
    const uint32_t capacity  = 0;
    std::vector<EntityId> entities;
    entities.reserve(1000);
    for (uint32_t i = 0; i < 1000; ++i) {
        entities.push_back(*f.mWorld.CreateEntity(mask));
    }
    (void)capacity;
    const uint32_t chunks = f.mWorld.GetStats().mChunks;
    CHECK(chunks >= 4);
    CHECK(chunks <= 6);
    CHECK(f.mWorld.GetEntityCount() == 1000);

    uint32_t rows       = 0;
    uint32_t seenChunks = 0;
    f.mWorld.ForEachChunk(mask, ComponentMask{}, [&](ChunkView& view) {
        ++seenChunks;
        rows += view.GetCount();
        const std::span<const Transform> transforms = view.Read<Transform>(f.mTransform);
        REQUIRE(transforms.size() == view.GetCount());
        CHECK(transforms[0].mMatrix[0] == 1.0f);
        CHECK(transforms[0].mMatrix[1] == 0.0f);
    });
    CHECK(rows == 1000);
    CHECK(seenChunks == chunks);

    for (const EntityId entity : entities) {
        REQUIRE(f.mWorld.DestroyEntity(entity).has_value());
    }
    CHECK(f.mWorld.GetStats().mChunks == 0);
    CHECK(f.mWorld.GetEntityCount() == 0);
    CHECK(f.mAllocator.GetStats().mLiveBytes > 0); // registries keep their tables
}

TEST_CASE("ForEachChunk filters by all and none and writes stamp the change version", "[ecs][world]") {
    Fixture f;
    const EntityId plain  = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mVelocity}));
    const EntityId frozen = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mVelocity, f.mFrozen}));
    const EntityId still  = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    (void)plain;
    (void)frozen;
    (void)still;

    uint32_t movable = 0;
    f.mWorld.ForEachChunk(MaskOf({f.mPosition, f.mVelocity}), MaskOf({f.mFrozen}),
                          [&movable](ChunkView& view) { movable += view.GetCount(); });
    CHECK(movable == 1);

    const uint32_t before = f.mWorld.GetVersion();
    f.mWorld.AdvanceVersion();
    f.mWorld.ForEachChunk(MaskOf({f.mVelocity}), ComponentMask{}, [&](ChunkView& view) {
        CHECK_FALSE(view.HasChangedSince(f.mVelocity, before));
        for (Velocity& velocity : view.Write<Velocity>(f.mVelocity)) {
            velocity.mZ = 1.0f;
        }
        CHECK(view.HasChangedSince(f.mVelocity, before));
        CHECK_FALSE(view.HasChangedSince(f.mPosition, before));
    });
    CHECK(f.mWorld.Get<Velocity>(plain)->mZ == 1.0f);
}

TEST_CASE("enable bits default on, follow the entity across archetypes and survive swaps", "[ecs][world]") {
    Fixture f;
    const EntityId first  = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mFrozen}));
    const EntityId second = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mFrozen}));
    CHECK(f.mWorld.IsComponentEnabled(first, f.mFrozen));
    f.mWorld.SetComponentEnabled(second, f.mFrozen, false);
    CHECK_FALSE(f.mWorld.IsComponentEnabled(second, f.mFrozen));
    CHECK_FALSE(f.mWorld.IsComponentEnabled(second, f.mVelocity)); // absent reads as disabled

    REQUIRE(f.mWorld.DestroyEntity(first).has_value()); // second swaps into row 0
    CHECK_FALSE(f.mWorld.IsComponentEnabled(second, f.mFrozen));
    CHECK(f.mWorld.IsComponentEnabled(second, f.mPosition));

    REQUIRE(f.mWorld.AddComponent(second, f.mVelocity).has_value());
    CHECK_FALSE(f.mWorld.IsComponentEnabled(second, f.mFrozen));
    CHECK(f.mWorld.IsComponentEnabled(second, f.mVelocity));

    f.mWorld.ForEachChunk(MaskOf({f.mFrozen}), ComponentMask{}, [&](ChunkView& view) {
        REQUIRE(view.GetCount() == 1);
        CHECK_FALSE(view.IsEnabled(0, f.mFrozen));
        CHECK(view.IsEnabled(0, f.mPosition));
    });
}

TEST_CASE("chunk components hold one value per chunk", "[ecs][world]") {
    Fixture f;
    const ComponentMask mask = MaskOf({f.mPosition, f.mBounds});
    const EntityId a         = *f.mWorld.CreateEntity(mask);
    const EntityId b         = *f.mWorld.CreateEntity(mask);
    CHECK(f.mWorld.GetChunkComponent(a, f.mBounds) == f.mWorld.GetChunkComponent(b, f.mBounds));
    CHECK(f.mWorld.GetChunkComponent(a, f.mPosition) == nullptr);
    CHECK(f.mWorld.Get<ChunkBounds>(a) == nullptr); // not a per-entity column

    auto* const bounds = static_cast<ChunkBounds*>(f.mWorld.GetChunkComponentForWrite(a, f.mBounds));
    REQUIRE(bounds != nullptr);
    bounds->mMax[0] = 9.0f;
    f.mWorld.ForEachChunk(MaskOf({f.mBounds}), ComponentMask{}, [&](ChunkView& view) {
        const auto* const seen = static_cast<const ChunkBounds*>(view.GetChunkComponent(f.mBounds));
        REQUIRE(seen != nullptr);
        CHECK(seen->mMax[0] == 9.0f);
    });
}

TEST_CASE("random structural changes agree with a reference model", "[ecs][world][stress]") {
    Fixture f;
    struct Model {
        bool mHasPosition = false;
        bool mHasVelocity = false;
        bool mHasHealth   = false;
        float mX          = 0.0f;
        uint16_t mCurrent = 100;
    };
    std::unordered_map<EntityId, Model> model;
    std::vector<EntityId> live;
    std::mt19937 rng(12345); // NOLINT(bugprone-random-generator-seed): the test must be reproducible

    const auto check = [&] {
        REQUIRE(f.mWorld.GetEntityCount() == live.size());
        for (const EntityId entity : live) {
            const Model& expected = model.at(entity);
            REQUIRE(f.mWorld.IsAlive(entity));
            REQUIRE(f.mWorld.HasComponent(entity, f.mPosition) == expected.mHasPosition);
            REQUIRE(f.mWorld.HasComponent(entity, f.mVelocity) == expected.mHasVelocity);
            REQUIRE(f.mWorld.HasComponent(entity, f.mHealth) == expected.mHasHealth);
            if (expected.mHasPosition) {
                REQUIRE(f.mWorld.Get<Position>(entity)->mX == expected.mX);
            }
            if (expected.mHasHealth) {
                REQUIRE(f.mWorld.Get<Health>(entity)->mCurrent == expected.mCurrent);
            }
        }
    };

    for (uint32_t step = 0; step < 4000; ++step) {
        const uint32_t action = rng() % 6;
        if (live.empty() || action == 0) {
            ComponentMask mask;
            Model entry;
            if (rng() % 2 == 0) {
                mask.Set(f.mPosition);
                entry.mHasPosition = true;
            }
            if (rng() % 2 == 0) {
                mask.Set(f.mHealth);
                entry.mHasHealth = true;
            }
            const EntityId entity = *f.mWorld.CreateEntity(mask);
            live.push_back(entity);
            model[entity] = entry;
            continue;
        }
        const size_t pick     = rng() % live.size();
        const EntityId entity = live[pick];
        Model& entry          = model[entity];
        switch (action) {
        case 1:
            REQUIRE(f.mWorld.DestroyEntity(entity).has_value());
            live[pick] = live.back();
            live.pop_back();
            model.erase(entity);
            break;
        case 2:
            if (!entry.mHasVelocity) {
                REQUIRE(f.mWorld.AddComponent(entity, f.mVelocity).has_value());
                entry.mHasVelocity = true;
            } else {
                REQUIRE(f.mWorld.RemoveComponent(entity, f.mVelocity).has_value());
                entry.mHasVelocity = false;
            }
            break;
        case 3:
            if (!entry.mHasPosition) {
                REQUIRE(f.mWorld.AddComponent(entity, f.mPosition).has_value());
                entry.mHasPosition = true;
                entry.mX           = 0.0f;
            } else {
                REQUIRE(f.mWorld.RemoveComponent(entity, f.mPosition).has_value());
                entry.mHasPosition = false;
            }
            break;
        case 4:
            if (entry.mHasPosition) {
                entry.mX                                   = static_cast<float>(rng() % 1000);
                f.mWorld.GetForWrite<Position>(entity)->mX = entry.mX;
            }
            break;
        default:
            if (entry.mHasHealth) {
                entry.mCurrent                                 = static_cast<uint16_t>(rng() % 200);
                f.mWorld.GetForWrite<Health>(entity)->mCurrent = entry.mCurrent;
            }
            break;
        }
        if (step % 500 == 0) {
            check();
        }
    }
    check();
    for (const EntityId entity : live) {
        REQUIRE(f.mWorld.DestroyEntity(entity).has_value());
    }
    CHECK(f.mWorld.GetStats().mChunks == 0);
}
