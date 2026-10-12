#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Memory.h>

#include <cstddef>
#include <mutex>
#include <span>

namespace oyi::core {

/// Frame memory (ADR-0009): fixed-size blocks handed out under a MemoryTag and released all at once
/// with FreeTag. Blocks come from the backing allocator on demand and are kept for reuse until the
/// heap is destroyed. Thread-safe; the hot path is FrameAllocator, which touches the heap only when
/// its current block is full.
class TaggedHeap {
public:
    struct Config {
        size_t mBlockSize      = size_t{1} << 20u; ///< bytes per block, a multiple of mBlockAlignment
        size_t mBlockAlignment = kCacheLineSize;   ///< alignment of block memory and of the usable area
    };

    struct Stats {
        size_t mFreeBlocks       = 0;
        size_t mUsedBlocks       = 0;
        size_t mLargeAllocations = 0; ///< requests served outside the block pool, see AcquireLarge
    };

    /// Uses the default Config: 1 MiB blocks aligned to a cache line.
    explicit TaggedHeap(Allocator& backing) noexcept;
    TaggedHeap(Allocator& backing, Config config) noexcept;
    TaggedHeap(const TaggedHeap&)            = delete;
    TaggedHeap(TaggedHeap&&)                 = delete;
    TaggedHeap& operator=(const TaggedHeap&) = delete;
    TaggedHeap& operator=(TaggedHeap&&)      = delete;
    /// Returns every block to the backing allocator, whether or not its tag was freed.
    ~TaggedHeap();

    /// Takes `blockCount` blocks from the backing allocator up front so that frames do not pay for
    /// growth. Fails with OutOfMemory when the backing allocator runs dry; blocks obtained before
    /// the failure are kept.
    [[nodiscard]] Result Reserve(size_t blockCount) noexcept;

    /// Hands out one block's usable area under `tag`; an empty span when the backing allocator is
    /// out of memory. The area is aligned to the block alignment.
    [[nodiscard]] std::span<std::byte> AcquireBlock(MemoryTag tag) noexcept;

    /// Serves a request that does not fit a block (size larger than the usable block size, or
    /// alignment above the block alignment) straight from the backing allocator, still owned by
    /// `tag`. nullptr when out of memory.
    [[nodiscard]] void* AcquireLarge(MemoryTag tag, size_t size, size_t alignment) noexcept;

    /// Releases everything acquired under `tag`: blocks go back to the free list, large allocations
    /// to the backing allocator. Pointers handed out under the tag become dangling.
    void FreeTag(MemoryTag tag) noexcept;

    /// Bytes available in each block after the block header.
    [[nodiscard]] size_t GetUsableBlockSize() const noexcept;
    [[nodiscard]] size_t GetBlockAlignment() const noexcept;
    [[nodiscard]] Stats GetStats() const noexcept;

private:
    struct BlockHeader;

    [[nodiscard]] static std::byte* UsableArea(BlockHeader* header, size_t headerBytes) noexcept;
    [[nodiscard]] BlockHeader* AllocateBlockLocked() noexcept;
    void ReleaseLocked(BlockHeader* header) noexcept;

    Allocator& mBacking;
    Config mConfig;
    size_t mHeaderBytes = 0;
    mutable std::mutex mMutex;
    BlockHeader* mFreeList = nullptr;
    BlockHeader* mUsedList = nullptr;
    Stats mStats;
};

} // namespace oyi::core
