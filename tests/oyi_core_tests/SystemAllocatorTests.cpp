#include <oyi_core/Memory.h>
#include <oyi_core/SystemAllocator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace {

using oyi::core::GetSystemAllocator;
using oyi::core::IsAligned;
using oyi::core::SystemAllocator;

struct Counted {
    explicit Counted(int& counter) noexcept : mCounter(counter) { ++mCounter; }
    Counted(const Counted&)            = delete;
    Counted(Counted&&)                 = delete;
    Counted& operator=(const Counted&) = delete;
    Counted& operator=(Counted&&)      = delete;
    ~Counted() { --mCounter; }

    int& mCounter;
};

} // namespace

TEST_CASE("SystemAllocator honours the requested alignment and tracks live memory", "[core][allocator]") {
    SystemAllocator allocator;
    for (const size_t alignment : {size_t{1}, size_t{8}, size_t{16}, size_t{64}, size_t{4096}}) {
        void* const memory = allocator.Allocate(100, alignment);
        REQUIRE(memory != nullptr);
        CHECK(IsAligned(memory, alignment));
        CHECK(allocator.GetStats().mLiveAllocations == 1);
        CHECK(allocator.GetStats().mLiveBytes == 100);
        allocator.Free(memory, 100, alignment);
        CHECK(allocator.GetStats().mLiveAllocations == 0);
        CHECK(allocator.GetStats().mLiveBytes == 0);
    }
}

TEST_CASE("SystemAllocator rejects a zero size and a non power of two alignment", "[core][allocator]") {
    SystemAllocator allocator;
    CHECK(allocator.Allocate(0, 8) == nullptr);
    CHECK(allocator.Allocate(8, 0) == nullptr);
    CHECK(allocator.Allocate(8, 24) == nullptr);
    allocator.Free(nullptr, 8, 8);
    CHECK(allocator.GetStats().mLiveAllocations == 0);
}

TEST_CASE("New and Delete construct and destroy through the allocator", "[core][allocator]") {
    SystemAllocator allocator;
    int liveObjects    = 0;
    auto* const object = allocator.New<Counted>(liveObjects);
    REQUIRE(object != nullptr);
    CHECK(liveObjects == 1);
    CHECK(allocator.GetStats().mLiveBytes == sizeof(Counted));
    allocator.Delete(object);
    CHECK(liveObjects == 0);
    CHECK(allocator.GetStats().mLiveAllocations == 0);
    allocator.Delete<Counted>(nullptr);
}

TEST_CASE("the process-wide system allocator is a single instance", "[core][allocator]") {
    CHECK(&GetSystemAllocator() == &GetSystemAllocator());
}
