#include <oyi_core/Expected.h>
#include <oyi_core/Memory.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ChunkLayout.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs_fixture/Components.gen.h>
#include <oyi_ecs_fixture/Components.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory_resource>

namespace {

using oyi::core::ErrorCode;
using oyi::core::IsAligned;
using oyi::core::kCacheLineSize;
using oyi::ecs::ChunkLayout;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentRegistry;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using namespace oyi::ecs_fixture;

constexpr uint32_t kChunkSize = 16 * 1024;

struct Registered {
    ComponentRegistry mRegistry;
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mVelocity;
    ComponentTypeIndex mHealth;
    ComponentTypeIndex mTransform;
    ComponentTypeIndex mFrozen;
    ComponentTypeIndex mBounds;

    Registered() {
        mPosition  = *mRegistry.Register<Position>();
        mVelocity  = *mRegistry.Register<Velocity>();
        mHealth    = *mRegistry.Register<Health>();
        mTransform = *mRegistry.Register<Transform>();
        mFrozen    = *mRegistry.Register<Frozen>();
        mBounds    = *mRegistry.Register<ChunkBounds>();
    }
};

} // namespace

TEST_CASE("ComponentRegistry hands out dense indices and finds types by TypeId", "[ecs][registry]") {
    const Registered r;
    CHECK(r.mPosition.mValue == 0);
    CHECK(r.mVelocity.mValue == 1);
    CHECK(r.mRegistry.GetCount() == 6);
    CHECK(r.mRegistry.Find<Health>() == r.mHealth);
    CHECK(r.mRegistry.Find(kHealthInfo.mTypeId) == r.mHealth);
    CHECK_FALSE(r.mRegistry.Find(oyi::core::MakeTypeId("nothing")).IsValid());
    CHECK(&r.mRegistry.GetInfo(r.mTransform) == &kTransformInfo);

    // Registering again is idempotent.
    Registered again;
    CHECK(*again.mRegistry.Register<Position>() == again.mPosition);
    CHECK(again.mRegistry.GetCount() == 6);
}

TEST_CASE("the generated default instance carries the declared member initializers", "[ecs][registry]") {
    REQUIRE(kHealthInfo.mDefault != nullptr);
    const auto* const health = static_cast<const Health*>(kHealthInfo.mDefault);
    CHECK(health->mCurrent == 100);
    CHECK(health->mMax == 100);
    CHECK_FALSE(health->mLastAttacker.IsValid());
    STATIC_REQUIRE(&oyi::core::ComponentTraits<Position>::kInfo == &kPositionInfo);
}

TEST_CASE("ChunkLayout packs SoA columns on cache lines and reports the capacity", "[ecs][layout]") {
    const Registered r;
    std::pmr::monotonic_buffer_resource resource;
    ComponentMask mask;
    mask.Set(r.mPosition);
    mask.Set(r.mVelocity);
    mask.Set(r.mHealth);

    const auto layout = ChunkLayout::Compute(r.mRegistry, mask, kChunkSize, &resource);
    REQUIRE(layout.has_value());
    CHECK(layout->GetTypeCount() == 3);
    CHECK(layout->GetColumns().size() == 3);
    CHECK(layout->GetChunkComponents().empty());
    CHECK(layout->GetUsedBytes() <= kChunkSize);
    // 8 (entity) + 12 + 12 + 12 bytes per row (EntityId is two uint32_t, so Health is 12 bytes):
    // 372 rows before alignment slack, a few less after.
    STATIC_REQUIRE(sizeof(Health) == 12);
    CHECK(layout->GetCapacity() >= 355);
    CHECK(layout->GetCapacity() <= 372);
    // No room for another row once the slack between columns is accounted for.
    CHECK(layout->GetUsedBytes() + 44 > kChunkSize - (4 * kCacheLineSize));

    CHECK(IsAligned(layout->GetEntityOffset(), static_cast<uint32_t>(kCacheLineSize)));
    for (const auto& column : layout->GetColumns()) {
        CHECK(IsAligned(column.mOffset, static_cast<uint32_t>(kCacheLineSize)));
        CHECK(column.mElementSize == r.mRegistry.GetInfo(column.mType).mSize);
        CHECK(layout->GetTypes()[column.mSlot] == column.mType);
    }
    CHECK(layout->FindColumn(r.mHealth) != nullptr);
    CHECK(layout->FindColumn(r.mTransform) == nullptr);
    CHECK(layout->FindSlot(r.mVelocity) == 1);
    CHECK(layout->FindSlot(r.mTransform) == 0xFFFF'FFFFu);
}

TEST_CASE("tags take no column and chunk components take one value per chunk", "[ecs][layout]") {
    const Registered r;
    std::pmr::monotonic_buffer_resource resource;
    ComponentMask mask;
    mask.Set(r.mTransform);
    mask.Set(r.mFrozen);
    mask.Set(r.mBounds);

    const auto layout = ChunkLayout::Compute(r.mRegistry, mask, kChunkSize, &resource);
    REQUIRE(layout.has_value());
    CHECK(layout->GetTypeCount() == 3);
    CHECK(layout->GetColumns().size() == 1);
    CHECK(layout->GetChunkComponents().size() == 1);
    CHECK(layout->FindChunkComponent(r.mBounds)->mSize == sizeof(ChunkBounds));
    CHECK(layout->FindColumn(r.mFrozen) == nullptr);
    CHECK(layout->FindSlot(r.mFrozen) != 0xFFFF'FFFFu);
    // 64-byte transforms plus 8-byte ids: a bit over 220 rows.
    CHECK(layout->GetCapacity() >= 215);
    CHECK(layout->GetCapacity() <= 227);
    CHECK(layout->GetEnableWordsPerType() == (layout->GetCapacity() + 63) / 64);
}

TEST_CASE("an empty mask still lays out the entity column", "[ecs][layout]") {
    const Registered r;
    std::pmr::monotonic_buffer_resource resource;
    const auto layout = ChunkLayout::Compute(r.mRegistry, ComponentMask{}, kChunkSize, &resource);
    REQUIRE(layout.has_value());
    CHECK(layout->GetTypeCount() == 0);
    CHECK(layout->GetCapacity() >= (kChunkSize - (2 * kCacheLineSize)) / sizeof(EntityId));
}

TEST_CASE("a chunk too small for one row is rejected", "[ecs][layout]") {
    const Registered r;
    std::pmr::monotonic_buffer_resource resource;
    ComponentMask mask;
    mask.Set(r.mTransform);
    const auto layout = ChunkLayout::Compute(r.mRegistry, mask, 256, &resource);
    REQUIRE_FALSE(layout.has_value());
    CHECK(layout.error().mCode == ErrorCode::InvalidArgument);

    ComponentMask unknown;
    unknown.Set(ComponentTypeIndex{.mValue = 42});
    const auto bad = ChunkLayout::Compute(r.mRegistry, unknown, kChunkSize, &resource);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().mCode == ErrorCode::InvalidArgument);
}
