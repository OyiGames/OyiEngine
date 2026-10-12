#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_ecs/ChunkLayout.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

/// All entities with exactly one set of component types, stored in fixed-size chunks (ADR-0005).
/// Chunks are kept packed: removing a row swaps the last row of the last chunk into the hole, and
/// an emptied chunk goes back to the allocator. Only World mutates an archetype.
class Archetype {
public:
    struct Chunk {
        std::byte* mData = nullptr;
        uint32_t mCount  = 0;
    };

    Archetype(uint32_t index, const ComponentMask& mask, ChunkLayout&& layout, const ComponentRegistry& registry,
              core::Allocator& chunkAllocator, std::pmr::memory_resource* resource) noexcept;
    Archetype(const Archetype&)            = delete;
    Archetype(Archetype&&)                 = delete;
    Archetype& operator=(const Archetype&) = delete;
    Archetype& operator=(Archetype&&)      = delete;
    ~Archetype();

    [[nodiscard]] uint32_t GetIndex() const noexcept { return mIndex; }
    [[nodiscard]] const ComponentMask& GetMask() const noexcept { return mMask; }
    [[nodiscard]] const ChunkLayout& GetLayout() const noexcept { return mLayout; }
    [[nodiscard]] uint32_t GetChunkCount() const noexcept { return static_cast<uint32_t>(mChunks.size()); }
    [[nodiscard]] uint32_t GetEntityCount() const noexcept { return mEntityCount; }
    [[nodiscard]] const Chunk& GetChunk(uint32_t chunk) const noexcept { return mChunks[chunk]; }

    /// Appends a row for `entity` with every component at its default, every type enabled and
    /// the chunk's versions stamped with `version`. Fails with OutOfMemory when a new chunk cannot
    /// be allocated.
    [[nodiscard]] core::Expected<EntityLocation> Allocate(EntityId entity, uint32_t version) noexcept;

    /// Frees the row at `location` by moving the last row into it. Returns the entity that moved
    /// (its location is now `location`), or an invalid id when the last row itself was removed.
    [[nodiscard]] EntityId Remove(EntityLocation location, uint32_t version) noexcept;

    /// Copies every component that both archetypes have from `sourceRow` into `targetRow`,
    /// enable bits included. Used when an entity changes archetype.
    void CopySharedComponents(const Archetype& source, EntityLocation sourceRow, EntityLocation targetRow) noexcept;

    [[nodiscard]] std::byte* GetColumn(uint32_t chunk, const ColumnLayout& column) noexcept;
    [[nodiscard]] const std::byte* GetColumn(uint32_t chunk, const ColumnLayout& column) const noexcept;
    [[nodiscard]] std::span<EntityId> GetEntities(uint32_t chunk) noexcept;
    [[nodiscard]] std::span<const EntityId> GetEntities(uint32_t chunk) const noexcept;
    [[nodiscard]] uint32_t GetVersion(uint32_t chunk, uint32_t slot) const noexcept;
    void SetVersion(uint32_t chunk, uint32_t slot, uint32_t version) noexcept;
    [[nodiscard]] bool IsEnabled(uint32_t chunk, uint32_t slot, uint32_t index) const noexcept;
    void SetEnabled(uint32_t chunk, uint32_t slot, uint32_t index, bool enabled) noexcept;
    [[nodiscard]] std::byte* GetChunkComponent(uint32_t chunk, const ChunkComponentLayout& component) noexcept;
    [[nodiscard]] const std::byte* GetChunkComponent(uint32_t chunk,
                                                     const ChunkComponentLayout& component) const noexcept;

private:
    [[nodiscard]] bool AllocateChunk(uint32_t version) noexcept;
    void ReleaseLastChunk() noexcept;
    void InitializeRow(EntityLocation row, uint32_t version) noexcept;
    void CopyRow(EntityLocation from, EntityLocation to) noexcept;

    uint32_t mIndex;
    ComponentMask mMask;
    ChunkLayout mLayout;
    const ComponentRegistry& mRegistry;
    core::Allocator& mChunkAllocator;
    std::pmr::vector<Chunk> mChunks;
    uint32_t mEntityCount = 0;
};

} // namespace oyi::ecs
