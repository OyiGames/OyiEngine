#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using oyi::core::SystemAllocator;
using oyi::ecs::EntityId;
using oyi::ecs::EntityLocation;
using oyi::ecs::EntityRegistry;
using oyi::ecs::kNoArchetype;

} // namespace

TEST_CASE("created entities are alive and distinct; destroyed ones stop resolving", "[ecs][entity]") {
    SystemAllocator allocator;
    EntityRegistry registry(allocator);

    const EntityId first  = registry.Create();
    const EntityId second = registry.Create();
    REQUIRE(first.IsValid());
    REQUIRE(second.IsValid());
    CHECK(first != second);
    CHECK(registry.IsAlive(first));
    CHECK(registry.IsAlive(second));
    CHECK(registry.GetAliveCount() == 2);
    CHECK(registry.GetLocation(first).mArchetype == kNoArchetype);

    registry.Destroy(first);
    CHECK_FALSE(registry.IsAlive(first));
    CHECK(registry.IsAlive(second));
    CHECK(registry.GetAliveCount() == 1);
    CHECK_FALSE(registry.IsAlive(EntityId::Invalid()));
    CHECK_FALSE(registry.IsAlive(EntityId{.mIndex = 1000, .mGeneration = 1}));
}

TEST_CASE("a recycled index carries a new generation", "[ecs][entity]") {
    SystemAllocator allocator;
    EntityRegistry registry(allocator);

    const EntityId original = registry.Create();
    registry.Destroy(original);
    const EntityId recycled = registry.Create();
    CHECK(recycled.mIndex == original.mIndex);
    CHECK(recycled.mGeneration == original.mGeneration + 1);
    CHECK(registry.IsAlive(recycled));
    CHECK_FALSE(registry.IsAlive(original));
    CHECK(registry.GetCapacity() == 1);
}

TEST_CASE("locations round-trip and are reset on destroy", "[ecs][entity]") {
    SystemAllocator allocator;
    EntityRegistry registry(allocator);
    const EntityId entity = registry.Create();
    registry.SetLocation(entity, EntityLocation{.mArchetype = 3, .mChunk = 2, .mIndex = 17});
    const EntityLocation location = registry.GetLocation(entity);
    CHECK(location.mArchetype == 3);
    CHECK(location.mChunk == 2);
    CHECK(location.mIndex == 17);

    registry.Destroy(entity);
    const EntityId next = registry.Create();
    CHECK(registry.GetLocation(next).mArchetype == kNoArchetype);
}

TEST_CASE("many create and destroy cycles keep every live handle unique", "[ecs][entity]") {
    SystemAllocator allocator;
    EntityRegistry registry(allocator);
    std::vector<EntityId> live;
    for (uint32_t round = 0; round < 50; ++round) {
        for (uint32_t i = 0; i < 100; ++i) {
            live.push_back(registry.Create());
        }
        for (uint32_t i = 0; i < 60; ++i) {
            registry.Destroy(live.back());
            live.pop_back();
        }
    }
    CHECK(registry.GetAliveCount() == live.size());
    for (size_t i = 0; i < live.size(); ++i) {
        CHECK(registry.IsAlive(live[i]));
        for (size_t j = i + 1; j < live.size(); ++j) {
            CHECK(live[i] != live[j]);
        }
    }
    CHECK(registry.GetCapacity() <= 100 + (40 * 50));
}
