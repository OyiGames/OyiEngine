#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Memory.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ChunkLayout.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource>
#include <span>
#include <utility>

namespace oyi::ecs {

Archetype::Archetype(uint32_t index, const ComponentMask& mask, ChunkLayout&& layout, const ComponentRegistry& registry,
                     core::Allocator& chunkAllocator, std::pmr::memory_resource* resource) noexcept
    : mIndex(index)
    , mMask(mask)
    , mLayout(std::move(layout))
    , mRegistry(registry)
    , mChunkAllocator(chunkAllocator)
    , mChunks(resource) {}

Archetype::~Archetype() {
    for (const Chunk& chunk : mChunks) {
        mChunkAllocator.Free(chunk.mData, mLayout.GetChunkSize(), core::kCacheLineSize);
    }
}

// --- raw access -------------------------------------------------------------------------------

std::byte* Archetype::GetColumn(uint32_t chunk, const ColumnLayout& column) noexcept {
    return mChunks[chunk].mData + column.mOffset;
}

const std::byte* Archetype::GetColumn(uint32_t chunk, const ColumnLayout& column) const noexcept {
    return mChunks[chunk].mData + column.mOffset;
}

std::span<EntityId> Archetype::GetEntities(uint32_t chunk) noexcept {
    auto* const entities = reinterpret_cast<EntityId*>(mChunks[chunk].mData + mLayout.GetEntityOffset());
    return {entities, mChunks[chunk].mCount};
}

std::span<const EntityId> Archetype::GetEntities(uint32_t chunk) const noexcept {
    const auto* const entities = reinterpret_cast<const EntityId*>(mChunks[chunk].mData + mLayout.GetEntityOffset());
    return {entities, mChunks[chunk].mCount};
}

uint32_t Archetype::GetVersion(uint32_t chunk, uint32_t slot) const noexcept {
    const auto* const versions = reinterpret_cast<const uint32_t*>(mChunks[chunk].mData + mLayout.GetVersionOffset());
    return versions[slot];
}

void Archetype::SetVersion(uint32_t chunk, uint32_t slot, uint32_t version) noexcept {
    auto* const versions = reinterpret_cast<uint32_t*>(mChunks[chunk].mData + mLayout.GetVersionOffset());
    versions[slot]       = version;
}

bool Archetype::IsEnabled(uint32_t chunk, uint32_t slot, uint32_t index) const noexcept {
    const auto* const words = reinterpret_cast<const uint64_t*>(mChunks[chunk].mData + mLayout.GetEnableOffset());
    const uint64_t word     = words[(slot * mLayout.GetEnableWordsPerType()) + (index / 64)];
    return (word & (uint64_t{1} << (index % 64))) != 0;
}

void Archetype::SetEnabled(uint32_t chunk, uint32_t slot, uint32_t index, bool enabled) noexcept {
    auto* const words  = reinterpret_cast<uint64_t*>(mChunks[chunk].mData + mLayout.GetEnableOffset());
    uint64_t& word     = words[(slot * mLayout.GetEnableWordsPerType()) + (index / 64)];
    const uint64_t bit = uint64_t{1} << (index % 64);
    word               = enabled ? (word | bit) : (word & ~bit);
}

std::byte* Archetype::GetChunkComponent(uint32_t chunk, const ChunkComponentLayout& component) noexcept {
    return mChunks[chunk].mData + component.mOffset;
}

const std::byte* Archetype::GetChunkComponent(uint32_t chunk, const ChunkComponentLayout& component) const noexcept {
    return mChunks[chunk].mData + component.mOffset;
}

// --- rows -------------------------------------------------------------------------------------

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
bool Archetype::AllocateChunk(uint32_t version) noexcept {
    void* memory = mChunkAllocator.Allocate(mLayout.GetChunkSize(), core::kCacheLineSize);
    if (memory == nullptr) {
        return false;
    }
    mChunks.push_back(Chunk{.mData = static_cast<std::byte*>(memory), .mCount = 0});
    const auto chunk = static_cast<uint32_t>(mChunks.size() - 1);
    for (uint32_t slot = 0; slot < mLayout.GetTypeCount(); ++slot) {
        SetVersion(chunk, slot, version);
    }
    // Enable bits start cleared; rows set their own bits when they are initialized.
    std::memset(mChunks[chunk].mData + mLayout.GetEnableOffset(), 0,
                static_cast<size_t>(mLayout.GetTypeCount()) * mLayout.GetEnableWordsPerType() * sizeof(uint64_t));
    for (const ChunkComponentLayout& component : mLayout.GetChunkComponents()) {
        const core::ComponentInfo& info = mRegistry.GetInfo(component.mType);
        std::byte* const target         = GetChunkComponent(chunk, component);
        if (info.mDefault != nullptr) {
            std::memcpy(target, info.mDefault, component.mSize);
        } else {
            std::memset(target, 0, component.mSize);
        }
    }
    return true;
}

void Archetype::ReleaseLastChunk() noexcept {
    OYI_ASSERT(!mChunks.empty(), "releasing a chunk from an empty archetype");
    OYI_ASSERT(mChunks.back().mCount == 0, "releasing a chunk that still has rows");
    mChunkAllocator.Free(mChunks.back().mData, mLayout.GetChunkSize(), core::kCacheLineSize);
    mChunks.pop_back();
}

void Archetype::InitializeRow(EntityLocation row, uint32_t version) noexcept {
    for (const ColumnLayout& column : mLayout.GetColumns()) {
        const core::ComponentInfo& info = mRegistry.GetInfo(column.mType);
        std::byte* const target =
            GetColumn(row.mChunk, column) + (static_cast<size_t>(row.mIndex) * column.mElementSize);
        if (info.mDefault != nullptr) {
            std::memcpy(target, info.mDefault, column.mElementSize);
        } else {
            std::memset(target, 0, column.mElementSize);
        }
        SetVersion(row.mChunk, column.mSlot, version);
    }
    for (uint32_t slot = 0; slot < mLayout.GetTypeCount(); ++slot) {
        SetEnabled(row.mChunk, slot, row.mIndex, true);
    }
}

void Archetype::CopyRow(EntityLocation from, EntityLocation to) noexcept {
    for (const ColumnLayout& column : mLayout.GetColumns()) {
        const std::byte* const source =
            GetColumn(from.mChunk, column) + (static_cast<size_t>(from.mIndex) * column.mElementSize);
        std::byte* const target = GetColumn(to.mChunk, column) + (static_cast<size_t>(to.mIndex) * column.mElementSize);
        std::memcpy(target, source, column.mElementSize);
    }
    for (uint32_t slot = 0; slot < mLayout.GetTypeCount(); ++slot) {
        SetEnabled(to.mChunk, slot, to.mIndex, IsEnabled(from.mChunk, slot, from.mIndex));
    }
    GetEntities(to.mChunk)[to.mIndex] = GetEntities(from.mChunk)[from.mIndex];
}

core::Expected<EntityLocation> Archetype::Allocate(EntityId entity, uint32_t version) noexcept {
    if (mChunks.empty() || mChunks.back().mCount == mLayout.GetCapacity()) {
        if (!AllocateChunk(version)) {
            return core::Fail(core::ErrorCode::OutOfMemory, "Archetype: cannot allocate a chunk");
        }
    }
    const auto chunk     = static_cast<uint32_t>(mChunks.size() - 1);
    const uint32_t index = mChunks[chunk].mCount;
    ++mChunks[chunk].mCount;
    ++mEntityCount;
    const EntityLocation row{.mArchetype = mIndex, .mChunk = chunk, .mIndex = index};
    InitializeRow(row, version);
    GetEntities(chunk)[index] = entity;
    return row;
}

EntityId Archetype::Remove(EntityLocation location, uint32_t version) noexcept {
    OYI_ASSERT(location.mArchetype == mIndex, "Archetype: removing a row of another archetype");
    OYI_ASSERT(location.mChunk < mChunks.size(), "Archetype: removing a row from a chunk that is not here");
    OYI_ASSERT(location.mIndex < mChunks[location.mChunk].mCount, "Archetype: removing a row past the chunk's rows");
    const auto lastChunk     = static_cast<uint32_t>(mChunks.size() - 1);
    const uint32_t lastIndex = mChunks[lastChunk].mCount - 1;
    EntityId moved           = EntityId::Invalid();
    if (location.mChunk != lastChunk || location.mIndex != lastIndex) {
        const EntityLocation last{.mArchetype = mIndex, .mChunk = lastChunk, .mIndex = lastIndex};
        CopyRow(last, location);
        moved = GetEntities(location.mChunk)[location.mIndex];
        // The hole's chunk changed for every type; the last chunk only lost its tail.
        for (uint32_t slot = 0; slot < mLayout.GetTypeCount(); ++slot) {
            SetVersion(location.mChunk, slot, version);
        }
    }
    --mChunks[lastChunk].mCount;
    --mEntityCount;
    if (mChunks[lastChunk].mCount == 0) {
        ReleaseLastChunk();
    }
    return moved;
}

void Archetype::CopySharedComponents(const Archetype& source, EntityLocation sourceRow,
                                     EntityLocation targetRow) noexcept {
    for (const ColumnLayout& column : mLayout.GetColumns()) {
        const ColumnLayout* const sourceColumn = source.mLayout.FindColumn(column.mType);
        if (sourceColumn == nullptr) {
            continue;
        }
        const std::byte* const from = source.GetColumn(sourceRow.mChunk, *sourceColumn) +
                                      (static_cast<size_t>(sourceRow.mIndex) * column.mElementSize);
        std::byte* const to =
            GetColumn(targetRow.mChunk, column) + (static_cast<size_t>(targetRow.mIndex) * column.mElementSize);
        std::memcpy(to, from, column.mElementSize);
    }
    for (uint32_t slot = 0; slot < mLayout.GetTypeCount(); ++slot) {
        const uint32_t sourceSlot = source.mLayout.FindSlot(mLayout.GetTypes()[slot]);
        if (sourceSlot == 0xFFFF'FFFFu) {
            continue;
        }
        SetEnabled(targetRow.mChunk, slot, targetRow.mIndex,
                   source.IsEnabled(sourceRow.mChunk, sourceSlot, sourceRow.mIndex));
    }
}

} // namespace oyi::ecs
