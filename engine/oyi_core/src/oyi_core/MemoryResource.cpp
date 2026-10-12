#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/MemoryResource.h>

#include <cstddef>
#include <memory_resource>

namespace oyi::core {

MemoryResource::MemoryResource(Allocator& allocator) noexcept : mAllocator(allocator) {}

Allocator& MemoryResource::GetAllocator() const noexcept {
    return mAllocator;
}

void* MemoryResource::do_allocate(size_t bytes, size_t alignment) {
    void* memory = mAllocator.Allocate(bytes, alignment);
    OYI_CHECK(memory != nullptr, "MemoryResource: the allocator is exhausted");
    return memory;
}

void MemoryResource::do_deallocate(void* ptr, size_t bytes, size_t alignment) {
    mAllocator.Free(ptr, bytes, alignment);
}

bool MemoryResource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
    return this == &other;
}

} // namespace oyi::core
