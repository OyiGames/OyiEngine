#include <oyi_core/Expected.h>
#include <oyi_core/LinearAllocator.h>
#include <oyi_core/Memory.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_core/TaggedHeap.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace {

using oyi::core::ErrorCode;
using oyi::core::IsAligned;
using oyi::core::kCacheLineSize;
using oyi::core::LinearAllocator;
using oyi::core::MemoryTag;
using oyi::core::SystemAllocator;
using oyi::core::TaggedHeap;

constexpr TaggedHeap::Config kSmallBlocks{.mBlockSize = 1024, .mBlockAlignment = kCacheLineSize};
constexpr MemoryTag kFrameA = MemoryTag::ForFrame(1);
constexpr MemoryTag kFrameB = MemoryTag::ForFrame(2);

} // namespace

TEST_CASE("TaggedHeap hands out aligned blocks and recycles them by tag", "[core][memory][taggedheap]") {
    SystemAllocator backing;
    TaggedHeap heap(backing, kSmallBlocks);
    CHECK(heap.GetUsableBlockSize() == 1024 - kCacheLineSize);
    CHECK(heap.GetBlockAlignment() == kCacheLineSize);

    const std::span<std::byte> first = heap.AcquireBlock(kFrameA);
    REQUIRE(first.size() == heap.GetUsableBlockSize());
    CHECK(IsAligned(first.data(), kCacheLineSize));
    const std::span<std::byte> second = heap.AcquireBlock(kFrameB);
    REQUIRE_FALSE(second.empty());
    CHECK(heap.GetStats().mUsedBlocks == 2);
    CHECK(backing.GetStats().mLiveAllocations == 2);

    heap.FreeTag(kFrameA);
    CHECK(heap.GetStats().mUsedBlocks == 1);
    CHECK(heap.GetStats().mFreeBlocks == 1);
    CHECK(backing.GetStats().mLiveAllocations == 2);

    const std::span<std::byte> reused = heap.AcquireBlock(kFrameB);
    CHECK(reused.data() == first.data());
    CHECK(heap.GetStats().mFreeBlocks == 0);

    heap.FreeTag(kFrameB);
    CHECK(heap.GetStats().mUsedBlocks == 0);
    CHECK(heap.GetStats().mFreeBlocks == 2);
}

TEST_CASE("Reserve takes blocks from the backing allocator up front", "[core][memory][taggedheap]") {
    SystemAllocator backing;
    TaggedHeap heap(backing, kSmallBlocks);
    REQUIRE(heap.Reserve(3).has_value());
    CHECK(heap.GetStats().mFreeBlocks == 3);
    CHECK(backing.GetStats().mLiveAllocations == 3);

    REQUIRE_FALSE(heap.AcquireBlock(kFrameA).empty());
    CHECK(backing.GetStats().mLiveAllocations == 3);
    CHECK(heap.GetStats().mFreeBlocks == 2);
    // Reserving fewer blocks than are already free is a no-op.
    REQUIRE(heap.Reserve(1).has_value());
    CHECK(heap.GetStats().mFreeBlocks == 2);
}

TEST_CASE("Reserve fails with OutOfMemory when the backing allocator is exhausted", "[core][memory][taggedheap]") {
    alignas(kCacheLineSize) std::array<std::byte, 2048> buffer{};
    LinearAllocator backing(std::span<std::byte>{buffer});
    TaggedHeap heap(backing, kSmallBlocks);

    const auto result = heap.Reserve(3);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().mCode == ErrorCode::OutOfMemory);
    CHECK(heap.GetStats().mFreeBlocks == 2);
    REQUIRE_FALSE(heap.AcquireBlock(kFrameA).empty());
    REQUIRE_FALSE(heap.AcquireBlock(kFrameA).empty());
    CHECK(heap.AcquireBlock(kFrameA).empty());
}

TEST_CASE("large allocations bypass the block pool and are released with their tag", "[core][memory][taggedheap]") {
    SystemAllocator backing;
    TaggedHeap heap(backing, kSmallBlocks);

    const void* const big = heap.AcquireLarge(kFrameA, 4096, 256);
    REQUIRE(big != nullptr);
    CHECK(IsAligned(big, size_t{256}));
    CHECK(heap.GetStats().mLargeAllocations == 1);
    CHECK(heap.GetStats().mUsedBlocks == 0);
    CHECK(backing.GetStats().mLiveAllocations == 1);
    CHECK(backing.GetStats().mLiveBytes >= 4096);

    CHECK(heap.AcquireLarge(kFrameA, 0, 256) == nullptr);
    CHECK(heap.AcquireLarge(kFrameA, 16, 3) == nullptr);

    heap.FreeTag(kFrameB);
    CHECK(heap.GetStats().mLargeAllocations == 1);
    heap.FreeTag(kFrameA);
    CHECK(heap.GetStats().mLargeAllocations == 0);
    CHECK(backing.GetStats().mLiveAllocations == 0);
}

TEST_CASE("destroying the heap returns every block, freed or not", "[core][memory][taggedheap]") {
    SystemAllocator backing;
    {
        TaggedHeap heap(backing, kSmallBlocks);
        REQUIRE(heap.Reserve(2).has_value());
        REQUIRE_FALSE(heap.AcquireBlock(kFrameA).empty());
        REQUIRE(heap.AcquireLarge(kFrameB, 8192, 64) != nullptr);
        CHECK(backing.GetStats().mLiveAllocations == 3);
    }
    CHECK(backing.GetStats().mLiveAllocations == 0);
    CHECK(backing.GetStats().mLiveBytes == 0);
}
