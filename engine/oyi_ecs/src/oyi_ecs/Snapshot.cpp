#include <oyi_core/Allocator.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ChunkLayout.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/World.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace oyi::ecs {

Snapshot::Snapshot(core::Allocator& allocator) noexcept
    : mResource(allocator), mEntities(&mResource), mRowByIndex(&mResource), mColumns(&mResource) {}

void Snapshot::Clear() noexcept {
    mEntities.clear();
    for (uint32_t& row : mRowByIndex) {
        row = kNoRow;
    }
    for (Column& column : mColumns) {
        column.mData.clear();
        column.mPresent.clear();
    }
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
void Snapshot::Capture(const World& world, const ComponentMask& types) noexcept {
    Clear();
    mVersion = world.GetVersion();
    if (mTypes != types) {
        // One column per requested type, in ascending type order. Kept across captures with the
        // same mask so that the column storage is reused.
        mTypes = types;
        mColumns.clear();
        types.ForEach([&](ComponentTypeIndex type) {
            Column column(&mResource);
            column.mType        = type;
            column.mElementSize = world.GetComponents().GetInfo(type).mSize;
            mColumns.push_back(column);
        });
    }

    for (uint32_t index = 0; index < world.GetArchetypeCount(); ++index) {
        const Archetype& archetype = world.GetArchetype(index);
        if (archetype.IsPrototype() || !archetype.GetMask().Intersects(types)) {
            continue;
        }
        for (uint32_t chunk = 0; chunk < archetype.GetChunkCount(); ++chunk) {
            const std::span<const EntityId> entities = archetype.GetEntities(chunk);
            for (uint32_t i = 0; i < entities.size(); ++i) {
                const EntityId entity = entities[i];
                const auto row        = static_cast<uint32_t>(mEntities.size());
                mEntities.push_back(entity);
                if (mRowByIndex.size() <= entity.mIndex) {
                    mRowByIndex.resize(static_cast<size_t>(entity.mIndex) + 1, kNoRow);
                }
                mRowByIndex[entity.mIndex] = row;
                for (Column& column : mColumns) {
                    const ColumnLayout* const layout = archetype.GetLayout().FindColumn(column.mType);
                    const size_t head                = column.mData.size();
                    column.mData.resize(head + column.mElementSize);
                    column.mPresent.push_back(layout != nullptr ? uint8_t{1} : uint8_t{0});
                    if (layout != nullptr) {
                        const std::byte* const source =
                            archetype.GetColumn(chunk, *layout) + (static_cast<size_t>(i) * column.mElementSize);
                        std::memcpy(column.mData.data() + head, source, column.mElementSize);
                    }
                }
            }
        }
    }
}

const Snapshot::Column* Snapshot::FindColumn(ComponentTypeIndex type) const noexcept {
    for (const Column& column : mColumns) {
        if (column.mType == type) {
            return &column;
        }
    }
    return nullptr;
}

uint32_t Snapshot::RowOf(EntityId entity) const noexcept {
    if (!entity.IsValid() || entity.mIndex >= mRowByIndex.size()) {
        return kNoRow;
    }
    const uint32_t row = mRowByIndex[entity.mIndex];
    // The index slot may have been recycled since the capture: the generation must match too.
    return row != kNoRow && mEntities[row] == entity ? row : kNoRow;
}

bool Snapshot::Contains(EntityId entity) const noexcept {
    return RowOf(entity) != kNoRow;
}

bool Snapshot::HasRow(uint32_t row, ComponentTypeIndex type) const noexcept {
    const Column* const column = FindColumn(type);
    return column != nullptr && row < column->mPresent.size() && column->mPresent[row] != 0;
}

const void* Snapshot::Get(EntityId entity, ComponentTypeIndex type) const noexcept {
    const uint32_t row = RowOf(entity);
    if (row == kNoRow || !HasRow(row, type)) {
        return nullptr;
    }
    const Column* const column = FindColumn(type);
    return column->mData.data() + (static_cast<size_t>(row) * column->mElementSize);
}

std::span<const std::byte> Snapshot::GetColumn(ComponentTypeIndex type) const noexcept {
    const Column* const column = FindColumn(type);
    return column == nullptr ? std::span<const std::byte>{} : std::span<const std::byte>{column->mData};
}

} // namespace oyi::ecs
