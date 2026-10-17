#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Query.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>

namespace {

using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::Query;
using oyi::ecs::World;
using namespace oyi::ecs_fixture;

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mFrozen   = *mWorld.GetComponents().Register<Frozen>();
};

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

} // namespace

TEST_CASE("a query caches its archetypes and picks up new ones as they appear", "[ecs][query]") {
    Fixture f;
    Query movable(f.mAllocator, MaskOf({f.mPosition, f.mVelocity}), MaskOf({f.mFrozen}));
    CHECK(movable.GetMatches(f.mWorld).empty());
    CHECK(movable.CountEntities(f.mWorld) == 0);

    (void)*f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    (void)*f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mVelocity}));
    (void)*f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mVelocity}));
    CHECK(movable.GetMatches(f.mWorld).size() == 1);
    CHECK(movable.CountEntities(f.mWorld) == 2);

    (void)*f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mVelocity, f.mFrozen}));
    CHECK(movable.GetMatches(f.mWorld).size() == 1); // excluded by `none`
    CHECK(movable.Matches(MaskOf({f.mPosition, f.mVelocity, f.mFrozen})) == false);

    uint32_t rows = 0;
    f.mWorld.ForEachChunk(movable, [&rows](ChunkView& view) { rows += view.GetCount(); });
    CHECK(rows == 2);
}

TEST_CASE("an empty query matches every archetype", "[ecs][query]") {
    Fixture f;
    (void)*f.mWorld.CreateEntity(ComponentMask{});
    (void)*f.mWorld.CreateEntity(MaskOf({f.mFrozen}));
    Query everything(f.mAllocator, ComponentMask{}, ComponentMask{});
    CHECK(everything.GetMatches(f.mWorld).size() == 2);
    CHECK(everything.CountEntities(f.mWorld) == 2);
}

TEST_CASE("a query never matches a prototype archetype", "[ecs][query][object]") {
    Fixture f;
    Query movable(f.mAllocator, MaskOf({f.mPosition, f.mVelocity}), ComponentMask{});
    const oyi::ecs::EntityId prototype = *f.mWorld.CreatePrototype(MaskOf({f.mPosition, f.mVelocity}));
    CHECK(movable.GetMatches(f.mWorld).empty());
    CHECK(movable.CountEntities(f.mWorld) == 0);
    (void)*f.mWorld.CloneEntity(prototype);
    (void)*f.mWorld.CloneEntity(prototype);
    CHECK(movable.GetMatches(f.mWorld).size() == 1);
    CHECK(movable.CountEntities(f.mWorld) == 2);
}
