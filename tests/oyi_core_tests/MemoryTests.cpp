#include <oyi_core/Memory.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace {

using oyi::core::AlignUp;
using oyi::core::IsAligned;
using oyi::core::IsValidAlignment;
using oyi::core::kCacheLineSize;
using oyi::core::MemoryTag;

} // namespace

TEST_CASE("AlignUp rounds integers up to the alignment", "[core][memory]") {
    STATIC_REQUIRE(AlignUp(size_t{0}, size_t{8}) == 0);
    STATIC_REQUIRE(AlignUp(size_t{1}, size_t{8}) == 8);
    STATIC_REQUIRE(AlignUp(size_t{8}, size_t{8}) == 8);
    STATIC_REQUIRE(AlignUp(size_t{13}, size_t{8}) == 16);
    STATIC_REQUIRE(AlignUp(uint32_t{100}, uint32_t{64}) == 128);
}

TEST_CASE("IsAligned and IsValidAlignment", "[core][memory]") {
    STATIC_REQUIRE(IsAligned(size_t{64}, size_t{64}));
    STATIC_REQUIRE_FALSE(IsAligned(size_t{65}, size_t{64}));
    STATIC_REQUIRE(IsValidAlignment(1));
    STATIC_REQUIRE(IsValidAlignment(kCacheLineSize));
    STATIC_REQUIRE_FALSE(IsValidAlignment(0));
    STATIC_REQUIRE_FALSE(IsValidAlignment(48));
}

TEST_CASE("AlignUp on pointers moves forward to the next aligned address", "[core][memory]") {
    alignas(kCacheLineSize) std::array<std::byte, 2 * kCacheLineSize> buffer{};
    std::byte* const base = buffer.data();
    CHECK(AlignUp(base, kCacheLineSize) == base);
    CHECK(AlignUp(base + 1, kCacheLineSize) == base + kCacheLineSize);
    CHECK(AlignUp(base + 1, size_t{1}) == base + 1);
    CHECK(IsAligned(AlignUp(base + 3, size_t{16}), size_t{16}));
}

TEST_CASE("MemoryTag is a value type built from a frame index", "[core][memory]") {
    STATIC_REQUIRE(MemoryTag::ForFrame(7) == MemoryTag{.mValue = 7});
    STATIC_REQUIRE(MemoryTag::ForFrame(7) != MemoryTag::ForFrame(8));
    STATIC_REQUIRE(MemoryTag::ForFrame(0x1'0000'0005ull) == MemoryTag::ForFrame(5));
}
