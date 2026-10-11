#include <oyi_core/Handle.h>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <unordered_set>

namespace {

struct EntityTag {};
struct ChunkTag {};
using EntityId = oyi::core::Handle<EntityTag>;
using ChunkId  = oyi::core::Handle<ChunkTag>;

} // namespace

TEST_CASE("default handle is invalid", "[core][handle]") {
    constexpr EntityId kId{};
    STATIC_CHECK(!kId.IsValid());
    STATIC_CHECK(kId == EntityId::Invalid());
}

TEST_CASE("generation zero is reserved for invalid", "[core][handle]") {
    constexpr EntityId kWithIndexOnly{.mIndex = 42, .mGeneration = 0};
    STATIC_CHECK(!kWithIndexOnly.IsValid());
    constexpr EntityId kLive{.mIndex = 42, .mGeneration = 1};
    STATIC_CHECK(kLive.IsValid());
}

TEST_CASE("same index with different generation are different handles", "[core][handle]") {
    constexpr EntityId kFirst{.mIndex = 7, .mGeneration = 1};
    constexpr EntityId kReused{.mIndex = 7, .mGeneration = 2};
    STATIC_CHECK(kFirst != kReused);
}

TEST_CASE("u64 packing round-trips", "[core][handle]") {
    constexpr EntityId kId{.mIndex = 0xDEAD'BEEFu, .mGeneration = 0x0000'00FFu};
    constexpr auto kPacked = kId.ToU64();
    STATIC_CHECK(kPacked == 0x0000'00FF'DEAD'BEEFull);
    STATIC_CHECK(EntityId::FromU64(kPacked) == kId);
}

TEST_CASE("handles are hashable", "[core][handle]") {
    std::unordered_set<EntityId> set;
    set.insert(EntityId{.mIndex = 1, .mGeneration = 1});
    set.insert(EntityId{.mIndex = 1, .mGeneration = 2});
    set.insert(EntityId{.mIndex = 1, .mGeneration = 1});
    CHECK(set.size() == 2);
}

TEST_CASE("different tags are different types", "[core][handle]") {
    STATIC_CHECK(!std::is_same_v<EntityId, ChunkId>);
    STATIC_CHECK(!std::is_convertible_v<EntityId, ChunkId>);
}
