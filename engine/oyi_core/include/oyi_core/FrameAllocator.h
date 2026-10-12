#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Memory.h>
#include <oyi_core/TaggedHeap.h>

#include <cstddef>

namespace oyi::core {

/// Per-worker bump allocator over TaggedHeap blocks (ADR-0009). Allocate is a pointer bump; a new
/// block is taken from the heap only when the current one is full. Free is a no-op: the frame's
/// memory is released by TaggedHeap::FreeTag. Single-threaded by contract; each worker owns one.
class FrameAllocator final : public Allocator {
public:
    FrameAllocator(TaggedHeap& heap, MemoryTag tag) noexcept;

    /// Returns nullptr only when the heap's backing allocator is out of memory.
    [[nodiscard]] void* Allocate(size_t size, size_t alignment) noexcept override;
    void Free(void* ptr, size_t size, size_t alignment) noexcept override;

    /// Switches to a new tag, typically at the start of a frame. The current block stays owned by
    /// the previous tag until that tag is freed on the heap.
    void SetTag(MemoryTag tag) noexcept;
    [[nodiscard]] MemoryTag GetTag() const noexcept;

private:
    TaggedHeap& mHeap;
    MemoryTag mTag;
    std::byte* mCursor = nullptr;
    std::byte* mEnd    = nullptr;
};

} // namespace oyi::core
