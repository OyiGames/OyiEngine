#pragma once

#include <oyi_ecs/AccessLog.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ChunkLayout.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace oyi::ecs {

/// One chunk as a system sees it (ADR-0005: chunk-level iteration, never a fake flat array).
/// Writing through a column stamps the chunk's change version for that type with the world
/// version the view was created with.
class ChunkView {
public:
    ChunkView(Archetype& archetype, uint32_t chunk, uint32_t version) noexcept
        : mArchetype(archetype), mChunk(chunk), mVersion(version) {}

    [[nodiscard]] const Archetype& GetArchetype() const noexcept { return mArchetype; }
    [[nodiscard]] uint32_t GetChunkIndex() const noexcept { return mChunk; }
    [[nodiscard]] uint32_t GetCount() const noexcept { return mArchetype.GetChunk(mChunk).mCount; }
    [[nodiscard]] std::span<const EntityId> GetEntities() const noexcept { return mArchetype.GetEntities(mChunk); }
    [[nodiscard]] bool Has(ComponentTypeIndex type) const noexcept { return mArchetype.GetMask().Test(type); }

    /// Element 0 of the column, or nullptr when the archetype lacks the type or it is a tag.
    [[nodiscard]] const void* GetColumn(ComponentTypeIndex type) const noexcept {
        const ColumnLayout* const column = mArchetype.GetLayout().FindColumn(type);
        if (column == nullptr) {
            return nullptr;
        }
        RecordAccess(mArchetype, mChunk, type, AccessKind::Read);
        return mArchetype.GetColumn(mChunk, *column);
    }

    /// Same as GetColumn and marks the type as changed in this chunk.
    [[nodiscard]] void* GetColumnForWrite(ComponentTypeIndex type) noexcept {
        const ColumnLayout* const column = mArchetype.GetLayout().FindColumn(type);
        if (column == nullptr) {
            return nullptr;
        }
        RecordAccess(mArchetype, mChunk, type, AccessKind::Write);
        mArchetype.SetVersion(mChunk, column->mSlot, mVersion);
        return mArchetype.GetColumn(mChunk, *column);
    }

    template <typename T>
    [[nodiscard]] std::span<const T> Read(ComponentTypeIndex type) const noexcept {
        const void* const column = GetColumn(type);
        return column == nullptr ? std::span<const T>{} : std::span<const T>{static_cast<const T*>(column), GetCount()};
    }

    template <typename T>
    [[nodiscard]] std::span<T> Write(ComponentTypeIndex type) noexcept {
        void* const column = GetColumnForWrite(type);
        return column == nullptr ? std::span<T>{} : std::span<T>{static_cast<T*>(column), GetCount()};
    }

    /// World version of the last write to `type` in this chunk; 0 when the type is absent.
    [[nodiscard]] uint32_t GetVersion(ComponentTypeIndex type) const noexcept {
        const uint32_t slot = mArchetype.GetLayout().FindSlot(type);
        return slot == 0xFFFF'FFFFu ? 0 : mArchetype.GetVersion(mChunk, slot);
    }

    /// Change filter: true when `type` was written after `sinceVersion`. May report chunks whose
    /// rows did not all change (the version is per chunk), never misses a change.
    [[nodiscard]] bool HasChangedSince(ComponentTypeIndex type, uint32_t sinceVersion) const noexcept {
        return GetVersion(type) > sinceVersion;
    }

    [[nodiscard]] bool IsEnabled(uint32_t index, ComponentTypeIndex type) const noexcept {
        const uint32_t slot = mArchetype.GetLayout().FindSlot(type);
        return slot != 0xFFFF'FFFFu && mArchetype.IsEnabled(mChunk, slot, index);
    }

    [[nodiscard]] const void* GetChunkComponent(ComponentTypeIndex type) const noexcept {
        const ChunkComponentLayout* const component = mArchetype.GetLayout().FindChunkComponent(type);
        if (component == nullptr) {
            return nullptr;
        }
        RecordAccess(mArchetype, mChunk, type, AccessKind::Read);
        return mArchetype.GetChunkComponent(mChunk, *component);
    }

    [[nodiscard]] void* GetChunkComponentForWrite(ComponentTypeIndex type) noexcept {
        const ChunkComponentLayout* const component = mArchetype.GetLayout().FindChunkComponent(type);
        if (component == nullptr) {
            return nullptr;
        }
        RecordAccess(mArchetype, mChunk, type, AccessKind::Write);
        mArchetype.SetVersion(mChunk, component->mSlot, mVersion);
        return mArchetype.GetChunkComponent(mChunk, *component);
    }

private:
    Archetype& mArchetype;
    uint32_t mChunk;
    uint32_t mVersion;
};

} // namespace oyi::ecs
