#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/Archetype.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/EntityRegistry.h>
#include <oyi_ecs/Query.h>

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

/// Entities, their components and the archetypes that store them (ADR-0004, ADR-0005). There is
/// no global world: client, server, editor, replay each own one.
///
/// Every operation here is immediate and single-threaded. Structural changes (create, destroy,
/// add, remove) move rows and invalidate component pointers, so during a frame they are issued
/// through the command buffer and replayed at a sync point (ADR-0008); direct calls are for setup,
/// loading and the sync point itself.
class World {
public:
    struct Config {
        uint32_t mChunkSize = 16 * 1024; ///< ADR-0005: 16 KB to start with, tuned on target phones
    };

    struct Stats {
        uint32_t mEntities   = 0;
        uint32_t mArchetypes = 0;
        uint32_t mChunks     = 0;
    };

    struct ReplayStats {
        uint32_t mApplied = 0;
        uint32_t mSkipped = 0; ///< commands aimed at dead entities, duplicate adds, missing removes
    };

    explicit World(core::Allocator& allocator) noexcept;
    World(core::Allocator& allocator, Config config) noexcept;
    World(const World&)            = delete;
    World(World&&)                 = delete;
    World& operator=(const World&) = delete;
    World& operator=(World&&)      = delete;
    ~World();

    [[nodiscard]] ComponentRegistry& GetComponents() noexcept { return mComponents; }
    [[nodiscard]] const ComponentRegistry& GetComponents() const noexcept { return mComponents; }

    // --- entities -------------------------------------------------------------------------------

    /// A live entity with the given components at their defaults. Fails with InvalidArgument for
    /// an unregistered type and OutOfMemory when storage cannot grow.
    [[nodiscard]] core::Expected<EntityId> CreateEntity(const ComponentMask& mask) noexcept;
    [[nodiscard]] core::Expected<EntityId> CreateEntity(std::span<const ComponentTypeIndex> types) noexcept;
    /// Fails with NotFound when the handle is stale.
    [[nodiscard]] core::Result DestroyEntity(EntityId entity) noexcept;
    [[nodiscard]] bool IsAlive(EntityId entity) const noexcept { return mEntities.IsAlive(entity); }
    [[nodiscard]] uint32_t GetEntityCount() const noexcept { return mEntities.GetAliveCount(); }

    // --- components -----------------------------------------------------------------------------

    /// Moves the entity to the archetype with `type` added, the new component at its default.
    /// Fails with NotFound for a dead entity, AlreadyExists when it already has the type.
    [[nodiscard]] core::Result AddComponent(EntityId entity, ComponentTypeIndex type) noexcept;
    /// Fails with NotFound for a dead entity or a type it does not have.
    [[nodiscard]] core::Result RemoveComponent(EntityId entity, ComponentTypeIndex type) noexcept;
    [[nodiscard]] bool HasComponent(EntityId entity, ComponentTypeIndex type) const noexcept;
    [[nodiscard]] const ComponentMask* GetComponentMask(EntityId entity) const noexcept;

    /// nullptr when the entity is dead, lacks the type, or the type is a tag or chunk component.
    /// Pointers are invalidated by any structural change.
    [[nodiscard]] const void* GetComponent(EntityId entity, ComponentTypeIndex type) const noexcept;
    /// Same as GetComponent and marks the type as changed in the entity's chunk.
    [[nodiscard]] void* GetComponentForWrite(EntityId entity, ComponentTypeIndex type) noexcept;

    template <typename T>
    [[nodiscard]] const T* Get(EntityId entity) const noexcept {
        return static_cast<const T*>(GetComponent(entity, mComponents.Find<T>()));
    }

    template <typename T>
    [[nodiscard]] T* GetForWrite(EntityId entity) noexcept {
        return static_cast<T*>(GetComponentForWrite(entity, mComponents.Find<T>()));
    }

    /// Enable bits (ADR-0005): the cheap way to switch a component off and on without moving
    /// the entity. Queries that honour the bit skip disabled rows. Dead entities and absent types
    /// read as disabled; setting them is a no-op.
    [[nodiscard]] bool IsComponentEnabled(EntityId entity, ComponentTypeIndex type) const noexcept;
    void SetComponentEnabled(EntityId entity, ComponentTypeIndex type, bool enabled) noexcept;

    /// The chunk component shared by every entity in `entity`'s chunk; nullptr when absent.
    [[nodiscard]] const void* GetChunkComponent(EntityId entity, ComponentTypeIndex type) const noexcept;
    [[nodiscard]] void* GetChunkComponentForWrite(EntityId entity, ComponentTypeIndex type) noexcept;

    // --- sync point (ADR-0008) ------------------------------------------------------------------

    /// Applies every command of every buffer in one deterministic order: ascending sort key, then
    /// buffer order, then recording order. Commands aimed at entities that are dead by the time
    /// they run are skipped and counted. The buffers are cleared afterwards.
    [[nodiscard]] ReplayStats ReplayCommands(std::span<CommandBuffer* const> buffers) noexcept;
    /// A full sync point: replay, then advance the world version for the next frame.
    [[nodiscard]] ReplayStats SyncPoint(std::span<CommandBuffer* const> buffers) noexcept;

    // --- change tracking ------------------------------------------------------------------------

    /// Writes stamp chunks with this version; the sync point advances it once per frame so that
    /// "changed since the version I last ran with" works for every system.
    [[nodiscard]] uint32_t GetVersion() const noexcept { return mVersion; }
    void AdvanceVersion() noexcept { ++mVersion; }

    // --- iteration ------------------------------------------------------------------------------

    /// Visits every chunk of every archetype that has all of `all` and none of `none`.
    /// `fn` receives a ChunkView&. Structural changes are not allowed inside.
    template <typename Fn>
    void ForEachChunk(const ComponentMask& all, const ComponentMask& none, Fn&& fn) {
        for (Archetype* const archetype : mArchetypes) {
            if (!archetype->GetMask().Contains(all) || archetype->GetMask().Intersects(none)) {
                continue;
            }
            for (uint32_t chunk = 0; chunk < archetype->GetChunkCount(); ++chunk) {
                ChunkView view(*archetype, chunk, mVersion);
                fn(view);
            }
        }
    }

    /// Visits every chunk of the archetypes a Query matches; the cache of the query is refreshed
    /// first.
    template <typename Fn>
    void ForEachChunk(Query& query, Fn&& fn) {
        for (const uint32_t index : query.GetMatches(*this)) {
            Archetype& archetype = *mArchetypes[index];
            for (uint32_t chunk = 0; chunk < archetype.GetChunkCount(); ++chunk) {
                ChunkView view(archetype, chunk, mVersion);
                fn(view);
            }
        }
    }

    [[nodiscard]] const Archetype* FindArchetype(const ComponentMask& mask) const noexcept;
    [[nodiscard]] const Archetype& GetArchetype(uint32_t index) const noexcept { return *mArchetypes[index]; }
    [[nodiscard]] uint32_t GetArchetypeCount() const noexcept { return static_cast<uint32_t>(mArchetypes.size()); }
    [[nodiscard]] Stats GetStats() const noexcept;

private:
    [[nodiscard]] core::Expected<Archetype*> GetOrCreateArchetype(const ComponentMask& mask) noexcept;
    [[nodiscard]] core::Result MoveEntity(EntityId entity, const ComponentMask& targetMask) noexcept;
    void RemoveRow(Archetype& archetype, EntityLocation row) noexcept;

    core::Allocator& mAllocator;
    Config mConfig;
    core::MemoryResource mResource;
    ComponentRegistry mComponents;
    EntityRegistry mEntities;
    std::pmr::vector<Archetype*> mArchetypes;
    uint32_t mVersion = 1;
};

} // namespace oyi::ecs
