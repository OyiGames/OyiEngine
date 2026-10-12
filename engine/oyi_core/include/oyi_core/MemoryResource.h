#pragma once

#include <oyi_core/Allocator.h>

#include <cstddef>
#include <memory_resource>

namespace oyi::core {

/// Lets std::pmr containers draw from an Allocator. The pmr contract has no way to report failure
/// without exceptions, so an exhausted allocator terminates through OYI_CHECK; use it with
/// allocators that are sized for their job, or call Allocator directly where failure is expected.
class MemoryResource final : public std::pmr::memory_resource {
public:
    explicit MemoryResource(Allocator& allocator) noexcept;

    [[nodiscard]] Allocator& GetAllocator() const noexcept;

private:
    [[nodiscard]] void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void* ptr, size_t bytes, size_t alignment) override;
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override;

    Allocator& mAllocator;
};

} // namespace oyi::core
