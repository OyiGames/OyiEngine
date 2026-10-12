#include <oyi_core/LinearAllocator.h>
#include <oyi_core/Memory.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace {

using oyi::core::IsAligned;
using oyi::core::kCacheLineSize;
using oyi::core::LinearAllocator;

} // namespace

TEST_CASE("LinearAllocator bumps through its buffer and reports usage", "[core][allocator]") {
    alignas(kCacheLineSize) std::array<std::byte, 256> buffer{};
    LinearAllocator allocator(std::span<std::byte>{buffer});
    CHECK(allocator.GetCapacity() == 256);

    const auto* const first = static_cast<const std::byte*>(allocator.Allocate(100, 16));
    REQUIRE(first == buffer.data());
    const auto* const second = static_cast<const std::byte*>(allocator.Allocate(10, 16));
    REQUIRE(second == buffer.data() + 112);
    CHECK(allocator.GetUsedBytes() == 122);
    CHECK(IsAligned(second, size_t{16}));
}

TEST_CASE("LinearAllocator returns nullptr when exhausted and Reset makes the buffer reusable", "[core][allocator]") {
    alignas(kCacheLineSize) std::array<std::byte, 64> buffer{};
    LinearAllocator allocator(std::span<std::byte>{buffer});

    CHECK(allocator.Allocate(64, 1) != nullptr);
    CHECK(allocator.Allocate(1, 1) == nullptr);
    CHECK(allocator.GetUsedBytes() == 64);

    allocator.Reset();
    CHECK(allocator.GetUsedBytes() == 0);
    CHECK(allocator.Allocate(1, 1) == buffer.data());
    // Padding that would push past the end counts as exhaustion too.
    CHECK(allocator.Allocate(63, 64) == nullptr);
}

TEST_CASE("LinearAllocator rejects a zero size and a bad alignment, and Free is a no-op", "[core][allocator]") {
    alignas(kCacheLineSize) std::array<std::byte, 64> buffer{};
    LinearAllocator allocator(std::span<std::byte>{buffer});
    CHECK(allocator.Allocate(0, 8) == nullptr);
    CHECK(allocator.Allocate(8, 12) == nullptr);
    void* const memory = allocator.Allocate(8, 8);
    allocator.Free(memory, 8, 8);
    CHECK(allocator.GetUsedBytes() == 8);
}
