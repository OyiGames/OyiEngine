#include <oyi_core/Profile.h>
#include <oyi_core/SystemAllocator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string_view>

#ifndef OYI_EXPECT_MIMALLOC
#define OYI_EXPECT_MIMALLOC 0
#endif

namespace {

/// The macros must be usable wherever a statement is, in both builds.
int Zoned(int value) {
    OYI_ZONE("Zoned");
    const std::string_view name = "zoned-text";
    OYI_ZONE_TEXT(name);
    OYI_FRAME_MARK;
    OYI_THREAD_NAME(std::string_view("oyi.test"));
    return value * 2;
}

} // namespace

TEST_CASE("the profiler macros compile to nothing or to zones and never change behaviour", "[core][profile]") {
    CHECK(Zoned(21) == 42);
    CHECK(oyi::core::IsProfilingEnabled() == (OYI_PROFILE != 0));
}

TEST_CASE("the system allocator reports the heap it runs on", "[core][allocator]") {
    const std::string_view backend = oyi::core::SystemAllocator::GetBackendName();
    CHECK((backend == "mimalloc" || backend == "crt"));
    CHECK((backend == "mimalloc") == (OYI_EXPECT_MIMALLOC != 0));
    oyi::core::SystemAllocator allocator;
    void* const block = allocator.Allocate(1000, 64);
    REQUIRE(block != nullptr);
    CHECK(reinterpret_cast<std::size_t>(block) % 64 == 0); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
    allocator.Free(block, 1000, 64);
    CHECK(allocator.GetStats().mLiveBytes == 0);
}
