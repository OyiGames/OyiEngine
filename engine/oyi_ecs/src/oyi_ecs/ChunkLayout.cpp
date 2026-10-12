#include <oyi_core/Expected.h>
#include <oyi_core/Memory.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/ChunkLayout.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>

#include <cstdint>
#include <memory_resource>

namespace oyi::ecs {

namespace {

constexpr uint32_t kNoSlot = 0xFFFF'FFFFu;

[[nodiscard]] uint32_t EnableWords(uint32_t capacity) noexcept {
    return (capacity + 63) / 64;
}

} // namespace

ChunkLayout::ChunkLayout(std::pmr::memory_resource* resource) noexcept
    : mTypes(resource), mColumns(resource), mChunkComponents(resource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<ChunkLayout> ChunkLayout::Compute(const ComponentRegistry& registry, const ComponentMask& mask,
                                                 uint32_t chunkSize, std::pmr::memory_resource* resource) noexcept {
    ChunkLayout layout(resource);
    layout.mChunkSize = chunkSize;

    bool unknown = false;
    mask.ForEach([&](ComponentTypeIndex type) {
        if (!registry.IsRegistered(type)) {
            unknown = true;
        }
        layout.mTypes.push_back(type);
    });
    if (unknown) {
        return core::Fail(core::ErrorCode::InvalidArgument, "ChunkLayout: mask names an unregistered type");
    }

    // Per-entity bytes decide the capacity; the rest is fixed or grows slowly with capacity.
    uint64_t bytesPerEntity = sizeof(EntityId);
    for (const ComponentTypeIndex type : layout.mTypes) {
        const core::ComponentInfo& info = registry.GetInfo(type);
        if (HasFlag(info.mFlags, core::ComponentFlags::Tag) || HasFlag(info.mFlags, core::ComponentFlags::Chunk)) {
            continue;
        }
        bytesPerEntity += info.mSize;
    }
    const auto typeCount = static_cast<uint32_t>(layout.mTypes.size());

    // Place everything for a candidate capacity; shrink until it fits.
    const auto tryLayout = [&](uint32_t capacity) -> uint32_t {
        layout.mColumns.clear();
        layout.mChunkComponents.clear();
        uint32_t offset       = 0;
        layout.mVersionOffset = offset;
        offset += typeCount * static_cast<uint32_t>(sizeof(uint32_t));
        offset                     = core::AlignUp(offset, static_cast<uint32_t>(alignof(uint64_t)));
        layout.mEnableOffset       = offset;
        layout.mEnableWordsPerType = EnableWords(capacity);
        offset += typeCount * layout.mEnableWordsPerType * static_cast<uint32_t>(sizeof(uint64_t));
        for (uint32_t slot = 0; slot < typeCount; ++slot) {
            const core::ComponentInfo& info = registry.GetInfo(layout.mTypes[slot]);
            if (!HasFlag(info.mFlags, core::ComponentFlags::Chunk)) {
                continue;
            }
            offset = core::AlignUp(offset, info.mAlign);
            layout.mChunkComponents.push_back(ChunkComponentLayout{
                .mType = layout.mTypes[slot], .mSlot = slot, .mOffset = offset, .mSize = info.mSize});
            offset += info.mSize;
        }
        offset               = core::AlignUp(offset, static_cast<uint32_t>(core::kCacheLineSize));
        layout.mEntityOffset = offset;
        offset += capacity * static_cast<uint32_t>(sizeof(EntityId));
        for (uint32_t slot = 0; slot < typeCount; ++slot) {
            const core::ComponentInfo& info = registry.GetInfo(layout.mTypes[slot]);
            if (HasFlag(info.mFlags, core::ComponentFlags::Tag) || HasFlag(info.mFlags, core::ComponentFlags::Chunk)) {
                continue;
            }
            offset = core::AlignUp(offset, static_cast<uint32_t>(core::kCacheLineSize));
            layout.mColumns.push_back(ColumnLayout{
                .mType = layout.mTypes[slot], .mSlot = slot, .mOffset = offset, .mElementSize = info.mSize});
            offset += capacity * info.mSize;
        }
        return offset;
    };

    // Alignment slack is at most one cache line per column plus the fixed area, so the first
    // guess is close; the loop below corrects it downwards.
    const uint64_t fixedGuess = (static_cast<uint64_t>(typeCount) * 16) + ((typeCount + 2) * core::kCacheLineSize);
    if (chunkSize <= fixedGuess) {
        return core::Fail(core::ErrorCode::InvalidArgument, "ChunkLayout: chunk too small for this archetype");
    }
    auto capacity = static_cast<uint32_t>((chunkSize - fixedGuess) / bytesPerEntity);
    while (capacity > 0 && tryLayout(capacity) > chunkSize) {
        --capacity;
    }
    if (capacity == 0) {
        return core::Fail(core::ErrorCode::InvalidArgument, "ChunkLayout: not even one entity fits in a chunk");
    }
    layout.mUsedBytes = tryLayout(capacity);
    layout.mCapacity  = capacity;
    return layout;
}

uint32_t ChunkLayout::FindSlot(ComponentTypeIndex type) const noexcept {
    for (uint32_t slot = 0; slot < mTypes.size(); ++slot) {
        if (mTypes[slot] == type) {
            return slot;
        }
    }
    return kNoSlot;
}

const ColumnLayout* ChunkLayout::FindColumn(ComponentTypeIndex type) const noexcept {
    for (const ColumnLayout& column : mColumns) {
        if (column.mType == type) {
            return &column;
        }
    }
    return nullptr;
}

const ChunkComponentLayout* ChunkLayout::FindChunkComponent(ComponentTypeIndex type) const noexcept {
    for (const ChunkComponentLayout& component : mChunkComponents) {
        if (component.mType == type) {
            return &component;
        }
    }
    return nullptr;
}

} // namespace oyi::ecs
