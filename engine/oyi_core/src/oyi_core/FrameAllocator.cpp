#include <oyi_core/FrameAllocator.h>
#include <oyi_core/Memory.h>
#include <oyi_core/TaggedHeap.h>

#include <cstddef>
#include <span>
#include <utility>

namespace oyi::core {

FrameAllocator::FrameAllocator(TaggedHeap& heap, MemoryTag tag) noexcept : mHeap(heap), mTag(tag) {}

void* FrameAllocator::Allocate(size_t size, size_t alignment) noexcept {
    if (size == 0 || !IsValidAlignment(alignment)) {
        return nullptr;
    }
    if (size > mHeap.GetUsableBlockSize() || alignment > mHeap.GetBlockAlignment()) {
        return mHeap.AcquireLarge(mTag, size, alignment);
    }
    // Blocks are aligned to the block alignment and their size is a multiple of it, so rounding the
    // cursor up for any smaller alignment never moves it past mEnd.
    std::byte* aligned = mCursor == nullptr ? nullptr : AlignUp(mCursor, alignment);
    if (aligned == nullptr || std::cmp_less(mEnd - aligned, size)) {
        const std::span<std::byte> block = mHeap.AcquireBlock(mTag);
        if (block.empty()) {
            return nullptr;
        }
        mCursor = block.data();
        mEnd    = mCursor + block.size();
        aligned = mCursor;
    }
    mCursor = aligned + size;
    return aligned;
}

void FrameAllocator::Free(void* /*ptr*/, size_t /*size*/, size_t /*alignment*/) noexcept {}

void FrameAllocator::SetTag(MemoryTag tag) noexcept {
    mTag    = tag;
    mCursor = nullptr;
    mEnd    = nullptr;
}

MemoryTag FrameAllocator::GetTag() const noexcept {
    return mTag;
}

} // namespace oyi::core
