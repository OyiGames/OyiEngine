#pragma once

#include <oyi_core/Expected.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

/// One SoA column inside a chunk: `mCapacity` elements of one per-entity component type.
struct ColumnLayout {
    ComponentTypeIndex mType;
    uint32_t mSlot        = 0; ///< position of the type in the archetype's ascending type list
    uint32_t mOffset      = 0; ///< byte offset of element 0 from the chunk start
    uint32_t mElementSize = 0;
};

/// One chunk component (ADR-0005): a single value per chunk, not per entity.
struct ChunkComponentLayout {
    ComponentTypeIndex mType;
    uint32_t mSlot   = 0;
    uint32_t mOffset = 0;
    uint32_t mSize   = 0;
};

/// Byte map of a chunk for one archetype (ADR-0005). From the chunk start:
///   change versions   uint32_t[typeCount]      world version of the last write per type
///   enable bits       uint64_t[typeCount][words] one bit per entity per type (ADR-0005 enable bits)
///   chunk components  one value per Chunk-flagged type
///   entity column     EntityId[capacity]
///   SoA columns       T[capacity] per per-entity type, each starting on a cache line
/// Tag components occupy no bytes; they only take part in versions and enable bits.
class ChunkLayout {
public:
    explicit ChunkLayout(std::pmr::memory_resource* resource) noexcept;

    /// Lays out `mask` inside `chunkSize` bytes. Fails with InvalidArgument when even one entity
    /// does not fit or when a type in the mask is not registered.
    [[nodiscard]] static core::Expected<ChunkLayout> Compute(const ComponentRegistry& registry,
                                                             const ComponentMask& mask, uint32_t chunkSize,
                                                             std::pmr::memory_resource* resource) noexcept;

    [[nodiscard]] uint32_t GetChunkSize() const noexcept { return mChunkSize; }
    [[nodiscard]] uint32_t GetCapacity() const noexcept { return mCapacity; }
    /// Every type in the mask, ascending; the index into this span is the type's slot.
    [[nodiscard]] std::span<const ComponentTypeIndex> GetTypes() const noexcept { return mTypes; }
    [[nodiscard]] uint32_t GetTypeCount() const noexcept { return static_cast<uint32_t>(mTypes.size()); }
    [[nodiscard]] std::span<const ColumnLayout> GetColumns() const noexcept { return mColumns; }
    [[nodiscard]] std::span<const ChunkComponentLayout> GetChunkComponents() const noexcept { return mChunkComponents; }
    [[nodiscard]] uint32_t GetVersionOffset() const noexcept { return mVersionOffset; }
    [[nodiscard]] uint32_t GetEnableOffset() const noexcept { return mEnableOffset; }
    [[nodiscard]] uint32_t GetEnableWordsPerType() const noexcept { return mEnableWordsPerType; }
    [[nodiscard]] uint32_t GetEntityOffset() const noexcept { return mEntityOffset; }
    /// Bytes actually used by the layout; the rest of the chunk is slack.
    [[nodiscard]] uint32_t GetUsedBytes() const noexcept { return mUsedBytes; }

    /// Slot of `type` in GetTypes(), or 0xFFFFFFFF when the type is not in the mask.
    [[nodiscard]] uint32_t FindSlot(ComponentTypeIndex type) const noexcept;
    [[nodiscard]] const ColumnLayout* FindColumn(ComponentTypeIndex type) const noexcept;
    [[nodiscard]] const ChunkComponentLayout* FindChunkComponent(ComponentTypeIndex type) const noexcept;

private:
    std::pmr::vector<ComponentTypeIndex> mTypes;
    std::pmr::vector<ColumnLayout> mColumns;
    std::pmr::vector<ChunkComponentLayout> mChunkComponents;
    uint32_t mChunkSize          = 0;
    uint32_t mCapacity           = 0;
    uint32_t mVersionOffset      = 0;
    uint32_t mEnableOffset       = 0;
    uint32_t mEnableWordsPerType = 0;
    uint32_t mEntityOffset       = 0;
    uint32_t mUsedBytes          = 0;
};

} // namespace oyi::ecs
