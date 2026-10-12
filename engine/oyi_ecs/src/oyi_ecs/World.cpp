#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ChunkLayout.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/World.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace oyi::ecs {

World::World(core::Allocator& allocator) noexcept : World(allocator, Config{}) {}

World::World(core::Allocator& allocator, Config config) noexcept
    : mAllocator(allocator), mConfig(config), mResource(allocator), mEntities(allocator), mArchetypes(&mResource) {
    const core::Expected<ResourceTypeIndex> environment =
        mResources.Register(kEnvironmentResourceName, mEnvironment, ResourceOrdering::Automatic);
    OYI_CHECK(environment.has_value() && *environment == kEnvironmentResource,
              "World: the Environment must be resource 0");
}

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

void* World::GetComponentForWrite(EntityId entity, ComponentTypeIndex type, uint32_t version) noexcept {
    if (!mEntities.IsAlive(entity)) {
        return nullptr;
    }
    const EntityLocation row         = mEntities.GetLocation(entity);
    Archetype& archetype             = *mArchetypes[row.mArchetype];
    const ColumnLayout* const column = archetype.GetLayout().FindColumn(type);
    if (column == nullptr) {
        return nullptr;
    }
    archetype.SetVersion(row.mChunk, column->mSlot, version);
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

// --- sync point -------------------------------------------------------------------------------

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
World::ReplayStats World::ReplayCommands(std::span<CommandBuffer* const> buffers) noexcept {
    struct Ref {
        uint64_t mSortKey = 0;
        uint32_t mBuffer  = 0;
        uint32_t mCommand = 0;
    };
    std::pmr::vector<Ref> order(&mResource);
    std::pmr::vector<std::pmr::vector<EntityId>> created(&mResource);
    for (uint32_t b = 0; b < buffers.size(); ++b) {
        const CommandBuffer& buffer = *buffers[b];
        for (uint32_t c = 0; c < buffer.mCommands.size(); ++c) {
            order.push_back(Ref{.mSortKey = buffer.mCommands[c].mSortKey, .mBuffer = b, .mCommand = c});
        }
        // The outer pmr vector hands its resource to the inner one (uses-allocator construction).
        created.emplace_back(buffer.mCreateMasks.size(), EntityId::Invalid());
    }
    // Gathering preserved buffer order and recording order; a stable sort by key keeps them as the
    // tie-breakers.
    std::ranges::stable_sort(order, [](const Ref& a, const Ref& b) { return a.mSortKey < b.mSortKey; });

    ReplayStats stats;
    for (const Ref& ref : order) {
        const CommandBuffer& buffer           = *buffers[ref.mBuffer];
        const CommandBuffer::Command& command = buffer.mCommands[ref.mCommand];
        if (command.mOp == CommandBuffer::Op::Create) {
            const core::Expected<EntityId> entity = CreateEntity(buffer.mCreateMasks[command.mPending.mIndex]);
            if (entity.has_value()) {
                created[ref.mBuffer][command.mPending.mIndex] = *entity;
                ++stats.mApplied;
            } else {
                ++stats.mSkipped;
            }
            continue;
        }
        const EntityId target =
            command.mPending.IsValid() ? created[ref.mBuffer][command.mPending.mIndex] : command.mEntity;
        if (!mEntities.IsAlive(target)) {
            ++stats.mSkipped;
            continue;
        }
        bool applied = false;
        switch (command.mOp) {
        case CommandBuffer::Op::Destroy:
            applied = DestroyEntity(target).has_value();
            break;
        case CommandBuffer::Op::Add:
            applied = AddComponent(target, command.mType).has_value();
            break;
        case CommandBuffer::Op::Remove:
            applied = RemoveComponent(target, command.mType).has_value();
            break;
        case CommandBuffer::Op::Set: {
            void* const component = GetComponentForWrite(target, command.mType);
            if (component != nullptr && mComponents.GetInfo(command.mType).mSize == command.mPayloadSize) {
                std::memcpy(component, buffer.mPayload.data() + command.mPayloadOffset, command.mPayloadSize);
                applied = true;
            }
            break;
        }
        case CommandBuffer::Op::Enable:
            if (HasComponent(target, command.mType)) {
                SetComponentEnabled(target, command.mType, command.mFlag);
                applied = true;
            }
            break;
        case CommandBuffer::Op::Create:
            break;
        }
        if (applied) {
            ++stats.mApplied;
        } else {
            ++stats.mSkipped;
        }
    }
    for (CommandBuffer* const buffer : buffers) {
        buffer->Clear();
    }
    return stats;
}

World::ReplayStats World::SyncPoint(std::span<CommandBuffer* const> buffers) noexcept {
    const ReplayStats stats = ReplayCommands(buffers);
    AdvanceVersion();
    return stats;
}

World::Stats World::GetStats() const noexcept {
    Stats stats{.mEntities = mEntities.GetAliveCount(), .mArchetypes = GetArchetypeCount(), .mChunks = 0};
    for (const Archetype* const archetype : mArchetypes) {
        stats.mChunks += archetype->GetChunkCount();
    }
    return stats;
}

} // namespace oyi::ecs
