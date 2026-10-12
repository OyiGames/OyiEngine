#include <oyi_core/FrameAllocator.h>
#include <oyi_core/Memory.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_core/TaggedHeap.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

namespace {

using oyi::core::FrameAllocator;
using oyi::core::IsAligned;
using oyi::core::kCacheLineSize;
using oyi::core::MemoryTag;
using oyi::core::SystemAllocator;
using oyi::core::TaggedHeap;

constexpr TaggedHeap::Config kSmallBlocks{.mBlockSize = 1024, .mBlockAlignment = kCacheLineSize};
constexpr MemoryTag kFrame = MemoryTag::ForFrame(1);

} // namespace

TEST_CASE("FrameAllocator bumps inside one block until it is full", "[core][memory][frameallocator]") {
    SystemAllocator backing;
    TaggedHeap heap(backing, kSmallBlocks);
    FrameAllocator frame(heap, kFrame);
    CHECK(frame.GetTag() == kFrame);

    auto* const first = static_cast<std::byte*>(frame.Allocate(100, 16));
    REQUIRE(first != nullptr);
    CHECK(IsAligned(first, kCacheLineSize));
    const auto* const second = static_cast<const std::byte*>(frame.Allocate(100, 16));
    CHECK(second == first + 112);
    CHECK(heap.GetStats().mUsedBlocks == 1);

    // 960 usable bytes per block: 216 are used, so one 600-byte request still fits and the next
    // two each open a new block.
    REQUIRE(frame.Allocate(600, 8) != nullptr);
    CHECK(heap.GetStats().mUsedBlocks == 1);
    REQUIRE(frame.Allocate(600, 8) != nullptr);
    CHECK(heap.GetStats().mUsedBlocks == 2);
    REQUIRE(frame.Allocate(600, 8) != nullptr);
    CHECK(heap.GetStats().mUsedBlocks == 3);
}

TEST_CASE("FrameAllocator sends oversized or over-aligned requests to the large path",
          "[core][memory][frameallocator]") {
    SystemAllocator backing;
    TaggedHeap heap(backing, kSmallBlocks);
    FrameAllocator frame(heap, kFrame);

    const void* const big = frame.Allocate(heap.GetUsableBlockSize() + 1, 8);
    REQUIRE(big != nullptr);
    const void* const page = frame.Allocate(16, 4096);
    REQUIRE(page != nullptr);
    CHECK(IsAligned(page, size_t{4096}));
    CHECK(heap.GetStats().mLargeAllocations == 2);
    CHECK(heap.GetStats().mUsedBlocks == 0);

    CHECK(frame.Allocate(0, 8) == nullptr);
    CHECK(frame.Allocate(8, 6) == nullptr);

    heap.FreeTag(kFrame);
    CHECK(heap.GetStats().mLargeAllocations == 0);
    CHECK(backing.GetStats().mLiveAllocations == 0);
}

TEST_CASE("Free is a no-op and SetTag starts the next frame on a fresh block", "[core][memory][frameallocator]") {
    SystemAllocator backing;
    TaggedHeap heap(backing, kSmallBlocks);
    FrameAllocator frame(heap, kFrame);

    void* const memory = frame.Allocate(64, 8);
    REQUIRE(memory != nullptr);
    frame.Free(memory, 64, 8);
    CHECK(frame.Allocate(64, 8) == static_cast<std::byte*>(memory) + 64);

    const MemoryTag nextFrame = MemoryTag::ForFrame(2);
    frame.SetTag(nextFrame);
    CHECK(frame.GetTag() == nextFrame);
    REQUIRE(frame.Allocate(64, 8) != nullptr);
    CHECK(heap.GetStats().mUsedBlocks == 2);

    heap.FreeTag(kFrame);
    CHECK(heap.GetStats().mUsedBlocks == 1);
    heap.FreeTag(nextFrame);
    CHECK(heap.GetStats().mUsedBlocks == 0);
}

TEST_CASE("several workers allocate from one heap under the same tag without overlap",
          "[core][memory][frameallocator]") {
    SystemAllocator backing;
    TaggedHeap heap(backing, kSmallBlocks);
    constexpr size_t kWorkers        = 4;
    constexpr size_t kAllocations    = 500;
    constexpr size_t kAllocationSize = 48;

    std::vector<std::vector<std::byte*>> perWorker(kWorkers);
    std::vector<std::thread> threads;
    threads.reserve(kWorkers);
    for (size_t worker = 0; worker < kWorkers; ++worker) {
        threads.emplace_back([&heap, &perWorker, worker] {
            FrameAllocator frame(heap, kFrame);
            const auto pattern = static_cast<std::byte>(worker + 1);
            for (size_t i = 0; i < kAllocations; ++i) {
                auto* const memory = static_cast<std::byte*>(frame.Allocate(kAllocationSize, 16));
                std::fill_n(memory, kAllocationSize, pattern);
                perWorker[worker].push_back(memory);
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    for (size_t worker = 0; worker < kWorkers; ++worker) {
        const auto pattern = static_cast<std::byte>(worker + 1);
        REQUIRE(perWorker[worker].size() == kAllocations);
        for (const std::byte* const memory : perWorker[worker]) {
            CHECK(std::all_of(memory, memory + kAllocationSize, [pattern](std::byte b) { return b == pattern; }));
        }
    }
    CHECK(heap.GetStats().mUsedBlocks >= kWorkers);
    heap.FreeTag(kFrame);
    CHECK(heap.GetStats().mUsedBlocks == 0);
}
