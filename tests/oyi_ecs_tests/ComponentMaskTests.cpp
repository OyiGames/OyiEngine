#include <oyi_ecs/ComponentMask.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {

using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::kMaxComponentTypes;

constexpr ComponentTypeIndex Type(uint16_t value) {
    return ComponentTypeIndex{.mValue = value};
}

constexpr ComponentMask MaskOf(std::initializer_list<uint16_t> types) {
    ComponentMask mask;
    for (const uint16_t type : types) {
        mask.Set(Type(type));
    }
    return mask;
}

} // namespace

TEST_CASE("ComponentTypeIndex defaults to invalid", "[ecs][mask]") {
    STATIC_REQUIRE_FALSE(ComponentTypeIndex{}.IsValid());
    STATIC_REQUIRE(Type(0).IsValid());
    STATIC_REQUIRE(Type(kMaxComponentTypes - 1).IsValid());
    STATIC_REQUIRE_FALSE(Type(kMaxComponentTypes).IsValid());
}

TEST_CASE("ComponentMask sets, clears and tests bits across all words", "[ecs][mask]") {
    constexpr ComponentMask kMask = MaskOf({0, 63, 64, 200, 255});
    STATIC_REQUIRE(kMask.Count() == 5);
    STATIC_REQUIRE(kMask.Test(Type(63)));
    STATIC_REQUIRE(kMask.Test(Type(64)));
    STATIC_REQUIRE_FALSE(kMask.Test(Type(65)));
    STATIC_REQUIRE_FALSE(kMask.IsEmpty());
    STATIC_REQUIRE(ComponentMask{}.IsEmpty());
    STATIC_REQUIRE(kMask.Without(Type(200)).Count() == 4);
    STATIC_REQUIRE(kMask.With(Type(1)).Test(Type(1)));
}

TEST_CASE("Contains and Intersects express subset and overlap", "[ecs][mask]") {
    constexpr ComponentMask kAll  = MaskOf({1, 2, 3, 100});
    constexpr ComponentMask kSome = MaskOf({2, 100});
    constexpr ComponentMask kNone = MaskOf({7, 101});
    STATIC_REQUIRE(kAll.Contains(kSome));
    STATIC_REQUIRE_FALSE(kSome.Contains(kAll));
    STATIC_REQUIRE(kAll.Contains(ComponentMask{}));
    STATIC_REQUIRE(kAll.Intersects(kSome));
    STATIC_REQUIRE_FALSE(kAll.Intersects(kNone));
    STATIC_REQUIRE(kAll == MaskOf({100, 3, 2, 1}));
    STATIC_REQUIRE(kAll != kSome);
}

TEST_CASE("ForEach visits set types in ascending order", "[ecs][mask]") {
    const ComponentMask mask = MaskOf({250, 5, 64, 0});
    std::vector<uint16_t> seen;
    mask.ForEach([&seen](ComponentTypeIndex type) { seen.push_back(type.mValue); });
    CHECK(seen == std::vector<uint16_t>{0, 5, 64, 250});
}
