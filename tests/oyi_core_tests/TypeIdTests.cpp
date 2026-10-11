#include <oyi_core/TypeId.h>

#include <catch2/catch_test_macros.hpp>

namespace {

using oyi::core::Fnv1a64;
using oyi::core::MakeTypeId;
using oyi::core::TypeId;

TEST_CASE("Fnv1a64 matches the reference vectors", "[core][typeid]") {
    // Reference values of 64-bit FNV-1a; tools/codegen/oyi_codegen/hashing.py must produce the same.
    STATIC_REQUIRE(Fnv1a64("") == 0xCBF2'9CE4'8422'2325ull);
    STATIC_REQUIRE(Fnv1a64("a") == 0xAF63'DC4C'8601'EC8Cull);
    STATIC_REQUIRE(Fnv1a64("foobar") == 0x8594'4171'F739'67E8ull);
    // Value produced by hashing.py for the same string.
    STATIC_REQUIRE(Fnv1a64("oyi::transform::Position") == 0x69B2'2D34'7756'711Bull);
}

TEST_CASE("TypeId is stable and distinguishes names", "[core][typeid]") {
    constexpr TypeId kPosition = MakeTypeId("oyi::transform::Position");
    STATIC_REQUIRE(kPosition.IsValid());
    STATIC_REQUIRE(kPosition == MakeTypeId("oyi::transform::Position"));
    STATIC_REQUIRE(kPosition != MakeTypeId("oyi::transform::Rotation"));
    STATIC_REQUIRE_FALSE(TypeId{}.IsValid());
}

} // namespace
