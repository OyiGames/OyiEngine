#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Memory.h>
#include <oyi_core/TaggedHeap.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <span>

namespace oyi::core {

/// Lives at the start of every block and every large allocation; the usable area follows it at the
/// block alignment. Blocks are linked into either the free list or the used list.
struct TaggedHeap::BlockHeader {
    BlockHeader* mNext = nullptr;
    MemoryTag mTag;
    bool mLarge        = false;
    size_t mTotalBytes = 0; ///< what was requested from the backing allocator, header included
    size_t mAlignment  = 0; ///< alignment used for that request
};

std::byte* TaggedHeap::UsableArea(BlockHeader* header, size_t headerBytes) noexcept {
    return reinterpret_cast<std::byte*>(header) + headerBytes;
}

TaggedHeap::TaggedHeap(Allocator& backing) noexcept : TaggedHeap(backing, Config{}) {}

TaggedHeap::TaggedHeap(Allocator& backing, Config config) noexcept : mBacking(backing), mConfig(config) {
    OYI_CHECK(IsValidAlignment(config.mBlockAlignment), "TaggedHeap: block alignment must be a power of two");
    OYI_CHECK(config.mBlockSize != 0, "TaggedHeap: block size must not be zero");
    OYI_CHECK(IsAligned(config.mBlockSize, config.mBlockAlignment),
              "TaggedHeap: block size must be a multiple of the block alignment");
    mHeaderBytes = AlignUp(sizeof(BlockHeader), config.mBlockAlignment);
    OYI_CHECK(config.mBlockSize > mHeaderBytes, "TaggedHeap: block size leaves no room after the header");
}

TaggedHeap::~TaggedHeap() {
    for (BlockHeader* list : {mFreeList, mUsedList}) {
        while (list != nullptr) {
            BlockHeader* const next = list->mNext;
            mBacking.Free(list, list->mTotalBytes, list->mAlignment);
            list = next;
        }
    }
}

TaggedHeap::BlockHeader* TaggedHeap::AllocateBlockLocked() noexcept {
    if (mFreeList != nullptr) {
        BlockHeader* const header = mFreeList;
        mFreeList                 = header->mNext;
        --mStats.mFreeBlocks;
        return header;
    }
    void* memory = mBacking.Allocate(mConfig.mBlockSize, mConfig.mBlockAlignment);
    if (memory == nullptr) {
        return nullptr;
    }
    return std::construct_at(static_cast<BlockHeader*>(memory), BlockHeader{.mNext       = nullptr,
                                                                            .mTag        = MemoryTag{},
                                                                            .mLarge      = false,
                                                                            .mTotalBytes = mConfig.mBlockSize,
                                                                            .mAlignment  = mConfig.mBlockAlignment});
}

void TaggedHeap::ReleaseLocked(BlockHeader* header) noexcept {
    if (header->mLarge) {
        --mStats.mLargeAllocations;
        mBacking.Free(header, header->mTotalBytes, header->mAlignment);
        return;
    }
    --mStats.mUsedBlocks;
    header->mNext = mFreeList;
    mFreeList     = header;
    ++mStats.mFreeBlocks;
}

Result TaggedHeap::Reserve(size_t blockCount) noexcept {
    const std::scoped_lock lock(mMutex);
    while (mStats.mFreeBlocks < blockCount) {
        void* memory = mBacking.Allocate(mConfig.mBlockSize, mConfig.mBlockAlignment);
        if (memory == nullptr) {
            return Fail(ErrorCode::OutOfMemory, "TaggedHeap::Reserve: backing allocator exhausted");
        }
        BlockHeader* const header =
            std::construct_at(static_cast<BlockHeader*>(memory), BlockHeader{.mNext       = mFreeList,
                                                                             .mTag        = MemoryTag{},
                                                                             .mLarge      = false,
                                                                             .mTotalBytes = mConfig.mBlockSize,
                                                                             .mAlignment  = mConfig.mBlockAlignment});
        mFreeList = header;
        ++mStats.mFreeBlocks;
    }
    return {};
}

std::span<std::byte> TaggedHeap::AcquireBlock(MemoryTag tag) noexcept {
    const std::scoped_lock lock(mMutex);
    BlockHeader* const header = AllocateBlockLocked();
    if (header == nullptr) {
        return {};
    }
    header->mTag  = tag;
    header->mNext = mUsedList;
    mUsedList     = header;
    ++mStats.mUsedBlocks;
    return {UsableArea(header, mHeaderBytes), mConfig.mBlockSize - mHeaderBytes};
}

void* TaggedHeap::AcquireLarge(MemoryTag tag, size_t size, size_t alignment) noexcept {
    if (size == 0 || !IsValidAlignment(alignment)) {
        return nullptr;
    }
    const size_t blockAlignment = alignment > mConfig.mBlockAlignment ? alignment : mConfig.mBlockAlignment;
    const size_t headerBytes    = AlignUp(sizeof(BlockHeader), blockAlignment);
    if (size > std::numeric_limits<size_t>::max() - headerBytes) {
        return nullptr;
    }
    const size_t totalBytes = headerBytes + size;

    const std::scoped_lock lock(mMutex);
    void* memory = mBacking.Allocate(totalBytes, blockAlignment);
    if (memory == nullptr) {
        return nullptr;
    }
    BlockHeader* const header = std::construct_at(
        static_cast<BlockHeader*>(memory),
        BlockHeader{
            .mNext = mUsedList, .mTag = tag, .mLarge = true, .mTotalBytes = totalBytes, .mAlignment = blockAlignment});
    mUsedList = header;
    ++mStats.mLargeAllocations;
    return UsableArea(header, headerBytes);
}

void TaggedHeap::FreeTag(MemoryTag tag) noexcept {
    const std::scoped_lock lock(mMutex);
    BlockHeader** link = &mUsedList;
    while (*link != nullptr) {
        BlockHeader* const header = *link;
        if (header->mTag != tag) {
            link = &header->mNext;
            continue;
        }
        *link = header->mNext;
        ReleaseLocked(header);
    }
}

size_t TaggedHeap::GetUsableBlockSize() const noexcept {
    return mConfig.mBlockSize - mHeaderBytes;
}

size_t TaggedHeap::GetBlockAlignment() const noexcept {
    return mConfig.mBlockAlignment;
}

TaggedHeap::Stats TaggedHeap::GetStats() const noexcept {
    const std::scoped_lock lock(mMutex);
    return mStats;
}

} // namespace oyi::core
