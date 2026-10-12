#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ChunkLayout.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>
#include <oyi_ecs/World.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace oyi::ecs {

World::World(core::Allocator& allocator) noexcept : World(allocator, Config{}) {}

World::World(core::Allocator& allocator, Config config) noexcept
    : mAllocator(allocator), mConfig(config), mResource(allocator), mEntities(allocator), mArchetypes(&mResource) {}

World::~World() {
    for (Archetype* const archetype : mArchetypes) {
        mAllocator.Delete(archetype);
    }
}

// --- archetypes -------------------------------------------------------------------------------

const Archetype* World::FindArchetype(const ComponentMask& mask) const noexcept {
    for (const Archetype* const archetype : mArchetypes) {
        if (archetype->GetMask() == mask) {
            return archetype;
        }
    }
    return nullptr;
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<Archetype*> World::GetOrCreateArchetype(const ComponentMask& mask) noexcept {
    const auto found =
        std::ranges::find_if(mArchetypes, [&mask](const Archetype* archetype) { return archetype->GetMask() == mask; });
    if (found != mArchetypes.end()) {
        return *found;
    }
    core::Expected<ChunkLayout> layout = ChunkLayout::Compute(mComponents, mask, mConfig.mChunkSize, &mResource);
    if (!layout.has_value()) {
        return core::Fail(layout.error().mCode, layout.error().mMessage);
    }
    const auto index = static_cast<uint32_t>(mArchetypes.size());
    mArchetypes.push_back(
        mAllocator.New<Archetype>(index, mask, std::move(*layout), mComponents, mAllocator, &mResource));
    if (mArchetypes.back() == nullptr) {
        mArchetypes.pop_back();
        return core::Fail(core::ErrorCode::OutOfMemory, "World: cannot allocate an archetype");
    }
    return mArchetypes.back();
}

// --- entities ---------------------------------------------------------------------------------

core::Expected<EntityId> World::CreateEntity(std::span<const ComponentTypeIndex> types) noexcept {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        if (!mComponents.IsRegistered(type)) {
            return core::Fail(core::ErrorCode::InvalidArgument, "World: unregistered component type");
        }
        mask.Set(type);
    }
    return CreateEntity(mask);
}

core::Expected<EntityId> World::CreateEntity(const ComponentMask& mask) noexcept {
    core::Expected<Archetype*> archetype = GetOrCreateArchetype(mask);
    if (!archetype.has_value()) {
        return core::Fail(archetype.error().mCode, archetype.error().mMessage);
    }
    const EntityId entity                    = mEntities.Create();
    const core::Expected<EntityLocation> row = (*archetype)->Allocate(entity, mVersion);
    if (!row.has_value()) {
        mEntities.Destroy(entity);
        return core::Fail(row.error().mCode, row.error().mMessage);
    }
    mEntities.SetLocation(entity, *row);
    return entity;
}

void World::RemoveRow(Archetype& archetype, EntityLocation row) noexcept {
    const EntityId moved = archetype.Remove(row, mVersion);
    if (moved.IsValid()) {
        mEntities.SetLocation(moved, row);
    }
}

core::Result World::DestroyEntity(EntityId entity) noexcept {
    if (!mEntities.IsAlive(entity)) {
        return core::Fail(core::ErrorCode::NotFound, "World: entity is not alive");
    }
    const EntityLocation row = mEntities.GetLocation(entity);
    RemoveRow(*mArchetypes[row.mArchetype], row);
    mEntities.Destroy(entity);
    return {};
}

// --- components -------------------------------------------------------------------------------

const ComponentMask* World::GetComponentMask(EntityId entity) const noexcept {
    if (!mEntities.IsAlive(entity)) {
        return nullptr;
    }
    return &mArchetypes[mEntities.GetLocation(entity).mArchetype]->GetMask();
}

bool World::HasComponent(EntityId entity, ComponentTypeIndex type) const noexcept {
    const ComponentMask* const mask = GetComponentMask(entity);
    return mask != nullptr && type.IsValid() && mask->Test(type);
}

core::Result World::MoveEntity(EntityId entity, const ComponentMask& targetMask) noexcept {
    core::Expected<Archetype*> target = GetOrCreateArchetype(targetMask);
    if (!target.has_value()) {
        return core::Fail(target.error().mCode, target.error().mMessage);
    }
    const EntityLocation sourceRow           = mEntities.GetLocation(entity);
    Archetype& source                        = *mArchetypes[sourceRow.mArchetype];
    const core::Expected<EntityLocation> row = (*target)->Allocate(entity, mVersion);
    if (!row.has_value()) {
        return core::Fail(row.error().mCode, row.error().mMessage);
    }
    (*target)->CopySharedComponents(source, sourceRow, *row);
    mEntities.SetLocation(entity, *row);
    RemoveRow(source, sourceRow);
    return {};
}

core::Result World::AddComponent(EntityId entity, ComponentTypeIndex type) noexcept {
    if (!mEntities.IsAlive(entity)) {
        return core::Fail(core::ErrorCode::NotFound, "World: entity is not alive");
    }
    if (!mComponents.IsRegistered(type)) {
        return core::Fail(core::ErrorCode::InvalidArgument, "World: unregistered component type");
    }
    const ComponentMask& current = mArchetypes[mEntities.GetLocation(entity).mArchetype]->GetMask();
    if (current.Test(type)) {
        return core::Fail(core::ErrorCode::AlreadyExists, "World: entity already has the component");
    }
    return MoveEntity(entity, current.With(type));
}

core::Result World::RemoveComponent(EntityId entity, ComponentTypeIndex type) noexcept {
    if (!mEntities.IsAlive(entity)) {
        return core::Fail(core::ErrorCode::NotFound, "World: entity is not alive");
    }
    const ComponentMask& current = mArchetypes[mEntities.GetLocation(entity).mArchetype]->GetMask();
    if (!type.IsValid() || !current.Test(type)) {
        return core::Fail(core::ErrorCode::NotFound, "World: entity does not have the component");
    }
    return MoveEntity(entity, current.Without(type));
}

const void* World::GetComponent(EntityId entity, ComponentTypeIndex type) const noexcept {
    if (!mEntities.IsAlive(entity)) {
        return nullptr;
    }
    const EntityLocation row         = mEntities.GetLocation(entity);
    const Archetype& archetype       = *mArchetypes[row.mArchetype];
    const ColumnLayout* const column = archetype.GetLayout().FindColumn(type);
    if (column == nullptr) {
        return nullptr;
    }
    return archetype.GetColumn(row.mChunk, *column) + (static_cast<size_t>(row.mIndex) * column->mElementSize);
}

void* World::GetComponentForWrite(EntityId entity, ComponentTypeIndex type) noexcept {
    if (!mEntities.IsAlive(entity)) {
        return nullptr;
    }
    const EntityLocation row         = mEntities.GetLocation(entity);
    Archetype& archetype             = *mArchetypes[row.mArchetype];
    const ColumnLayout* const column = archetype.GetLayout().FindColumn(type);
    if (column == nullptr) {
        return nullptr;
    }
    archetype.SetVersion(row.mChunk, column->mSlot, mVersion);
    return archetype.GetColumn(row.mChunk, *column) + (static_cast<size_t>(row.mIndex) * column->mElementSize);
}

bool World::IsComponentEnabled(EntityId entity, ComponentTypeIndex type) const noexcept {
    if (!mEntities.IsAlive(entity)) {
        return false;
    }
    const EntityLocation row   = mEntities.GetLocation(entity);
    const Archetype& archetype = *mArchetypes[row.mArchetype];
    const uint32_t slot        = archetype.GetLayout().FindSlot(type);
    return slot != 0xFFFF'FFFFu && archetype.IsEnabled(row.mChunk, slot, row.mIndex);
}

void World::SetComponentEnabled(EntityId entity, ComponentTypeIndex type, bool enabled) noexcept {
    if (!mEntities.IsAlive(entity)) {
        return;
    }
    const EntityLocation row = mEntities.GetLocation(entity);
    Archetype& archetype     = *mArchetypes[row.mArchetype];
    const uint32_t slot      = archetype.GetLayout().FindSlot(type);
    if (slot == 0xFFFF'FFFFu) {
        return;
    }
    archetype.SetEnabled(row.mChunk, slot, row.mIndex, enabled);
    archetype.SetVersion(row.mChunk, slot, mVersion);
}

const void* World::GetChunkComponent(EntityId entity, ComponentTypeIndex type) const noexcept {
    if (!mEntities.IsAlive(entity)) {
        return nullptr;
    }
    const EntityLocation row                    = mEntities.GetLocation(entity);
    const Archetype& archetype                  = *mArchetypes[row.mArchetype];
    const ChunkComponentLayout* const component = archetype.GetLayout().FindChunkComponent(type);
    return component == nullptr ? nullptr : archetype.GetChunkComponent(row.mChunk, *component);
}

void* World::GetChunkComponentForWrite(EntityId entity, ComponentTypeIndex type) noexcept {
    if (!mEntities.IsAlive(entity)) {
        return nullptr;
    }
    const EntityLocation row                    = mEntities.GetLocation(entity);
    Archetype& archetype                        = *mArchetypes[row.mArchetype];
    const ChunkComponentLayout* const component = archetype.GetLayout().FindChunkComponent(type);
    if (component == nullptr) {
        return nullptr;
    }
    archetype.SetVersion(row.mChunk, component->mSlot, mVersion);
    return archetype.GetChunkComponent(row.mChunk, *component);
}

World::Stats World::GetStats() const noexcept {
    Stats stats{.mEntities = mEntities.GetAliveCount(), .mArchetypes = GetArchetypeCount(), .mChunks = 0};
    for (const Archetype* const archetype : mArchetypes) {
        stats.mChunks += archetype->GetChunkCount();
    }
    return stats;
}

} // namespace oyi::ecs
