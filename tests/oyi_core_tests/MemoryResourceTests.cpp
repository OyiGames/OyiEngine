#include <oyi_core/MemoryResource.h>
#include <oyi_core/SystemAllocator.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory_resource>
#include <vector>

namespace {

using oyi::core::MemoryResource;
using oyi::core::SystemAllocator;

} // namespace

TEST_CASE("std::pmr containers allocate through a MemoryResource and free everything on destruction",
          "[core][allocator][pmr]") {
    SystemAllocator allocator;
    MemoryResource resource(allocator);
    CHECK(&resource.GetAllocator() == &allocator);
    {
        std::pmr::vector<uint32_t> values(&resource);
        for (uint32_t i = 0; i < 1000; ++i) {
            values.push_back(i);
        }
        // Debug standard libraries may allocate bookkeeping objects through the resource as well.
        CHECK(allocator.GetStats().mLiveAllocations >= 1);
        CHECK(allocator.GetStats().mLiveBytes >= 1000 * sizeof(uint32_t));
        CHECK(values[999] == 999);
    }
    CHECK(allocator.GetStats().mLiveAllocations == 0);
    CHECK(allocator.GetStats().mLiveBytes == 0);
}

TEST_CASE("a MemoryResource is only equal to itself", "[core][allocator][pmr]") {
    SystemAllocator allocator;
    MemoryResource first(allocator);
    MemoryResource second(allocator);
    CHECK(first.is_equal(first));
    CHECK_FALSE(first.is_equal(second));
}
