#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_net/Prediction.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::World;
using oyi::net::InputHistory;
using oyi::net::PredictionHistory;
using namespace oyi::ecs_fixture;

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

struct Move {
    float mDx = 0.0f;
};

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mNetPose  = *mWorld.GetComponents().Register<NetPose>();
    PredictionHistory mHistory{mAllocator, 4};
    std::vector<EntityId> mEntities;

    Fixture() {
        mHistory.Build(mWorld.GetComponents());
        for (uint32_t i = 0; i < 100; ++i) {
            mEntities.push_back(*mWorld.CreateEntity(MaskOf({mPosition, mNetPose})));
        }
        mEntities.push_back(*mWorld.CreateEntity(MaskOf({mPosition}))); // nothing predicted here
    }

    /// One predicted tick: every NetPose moves by dx, every Position by 1 (not predicted).
    void Step(float dx) {
        for (const EntityId entity : mEntities) {
            if (auto* const pose = mWorld.GetForWrite<NetPose>(entity); pose != nullptr) {
                pose->mX += dx;
            }
            if (auto* const position = mWorld.GetForWrite<Position>(entity); position != nullptr) {
                position->mX += 1.0f;
            }
        }
    }
};

} // namespace

TEST_CASE("the history keeps the predicted types of the last N ticks and rolls them back", "[net][prediction]") {
    Fixture f;
    CHECK(f.mHistory.GetPredictedTypes().Test(f.mNetPose));
    CHECK_FALSE(f.mHistory.GetPredictedTypes().Test(f.mPosition));
    CHECK(f.mHistory.GetCapacity() == 4);
    CHECK(f.mHistory.GetCount() == 0);

    for (uint64_t tick = 1; tick <= 6; ++tick) {
        f.Step(1.0f);
        f.mHistory.Record(f.mWorld, tick);
    }
    // Six ticks recorded into four slots: 3..6 remain.
    CHECK(f.mHistory.GetCount() == 4);
    CHECK(f.mHistory.GetOldestTick() == 3);
    CHECK(f.mHistory.GetNewestTick() == 6);
    CHECK_FALSE(f.mHistory.Contains(2));
    CHECK(f.mHistory.Contains(3));
    CHECK(f.mWorld.Get<NetPose>(f.mEntities[0])->mX == 6.0f);
    CHECK(f.mWorld.Get<Position>(f.mEntities[0])->mX == 6.0f);

    const auto stats = f.mHistory.Rollback(f.mWorld, 3);
    REQUIRE(stats.has_value());
    CHECK(stats->mRestored == 100);
    CHECK(stats->mSkipped == 0);
    CHECK(f.mWorld.Get<NetPose>(f.mEntities[0])->mX == 3.0f);
    CHECK(f.mWorld.Get<NetPose>(f.mEntities[99])->mX == 3.0f);
    CHECK(f.mWorld.Get<Position>(f.mEntities[0])->mX == 6.0f); // not predicted: left alone
    CHECK(f.mHistory.Rollback(f.mWorld, 2).error().mCode == ErrorCode::NotFound);
    CHECK(f.mHistory.Rollback(f.mWorld, 7).error().mCode == ErrorCode::NotFound);

    // Resimulating from 3 overwrites the stale captures of 4..6.
    f.mHistory.Truncate(3);
    CHECK(f.mHistory.GetCount() == 1);
    CHECK(f.mHistory.GetNewestTick() == 3);
    for (uint64_t tick = 4; tick <= 6; ++tick) {
        f.Step(2.0f);
        f.mHistory.Record(f.mWorld, tick);
    }
    CHECK(f.mHistory.GetCount() == 4);
    CHECK(f.mWorld.Get<NetPose>(f.mEntities[0])->mX == 9.0f);
    REQUIRE(f.mHistory.Rollback(f.mWorld, 5).has_value());
    CHECK(f.mWorld.Get<NetPose>(f.mEntities[0])->mX == 7.0f);
}

TEST_CASE("a rollback skips entities that died or lost the type since the capture", "[net][prediction]") {
    Fixture f;
    f.Step(1.0f);
    f.mHistory.Record(f.mWorld, 10);
    REQUIRE(f.mWorld.DestroyEntity(f.mEntities[0]).has_value());
    REQUIRE(f.mWorld.RemoveComponent(f.mEntities[1], f.mNetPose).has_value());
    f.Step(1.0f);
    const auto stats = f.mHistory.Rollback(f.mWorld, 10);
    REQUIRE(stats.has_value());
    CHECK(stats->mRestored == 98);
    CHECK(stats->mSkipped == 2);
    CHECK(f.mWorld.Get<NetPose>(f.mEntities[2])->mX == 1.0f);
    // A recycled handle is not the captured entity.
    const EntityId reborn = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mNetPose}));
    CHECK(reborn.mIndex == f.mEntities[0].mIndex);
    CHECK(reborn != f.mEntities[0]);
    f.mWorld.GetForWrite<NetPose>(reborn)->mX = 50.0f;
    REQUIRE(f.mHistory.Rollback(f.mWorld, 10).has_value());
    CHECK(f.mWorld.Get<NetPose>(reborn)->mX == 50.0f);
}

TEST_CASE("the input history hands back the input of a tick while it is in the ring", "[net][prediction]") {
    SystemAllocator allocator;
    InputHistory<Move> inputs(allocator, 3);
    CHECK(inputs.GetCapacity() == 3);
    CHECK(inputs.Get(1) == nullptr);
    inputs.Push(1, Move{.mDx = 1.0f});
    inputs.Push(2, Move{.mDx = 2.0f});
    inputs.Push(3, Move{.mDx = 3.0f});
    inputs.Push(4, Move{.mDx = 4.0f}); // evicts tick 1
    CHECK(inputs.Get(1) == nullptr);
    REQUIRE(inputs.Get(2) != nullptr);
    CHECK(inputs.Get(2)->mDx == 2.0f);
    CHECK(inputs.Get(4)->mDx == 4.0f);
    inputs.Push(2, Move{.mDx = 20.0f}); // a tick may be rewritten
    CHECK(inputs.Get(2)->mDx == 20.0f);
}
