#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/Entity.h>

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

inline constexpr uint32_t kNoArchetype = 0xFFFF'FFFFu;

/// Where an entity's components live: archetype, chunk within it, row within the chunk. Rows move
/// when other entities are removed (swap-remove), so locations are looked up, never stored.
struct EntityLocation {
    uint32_t mArchetype = kNoArchetype;
    uint32_t mChunk     = 0;
    uint32_t mIndex     = 0;
};

/// Allocates EntityIds with generations (ADR-0004) and tracks each live entity's location.
/// Indices are recycled through a free list; the generation of a recycled index is incremented so
/// handles to the old entity stop resolving.
class EntityRegistry {
public:
    explicit EntityRegistry(core::Allocator& allocator) noexcept;
    EntityRegistry(const EntityRegistry&)            = delete;
    EntityRegistry(EntityRegistry&&)                 = delete;
    EntityRegistry& operator=(const EntityRegistry&) = delete;
    EntityRegistry& operator=(EntityRegistry&&)      = delete;
    ~EntityRegistry()                                = default;

    /// A live entity with no location yet. The slot table grows through the allocator; running
    /// out of memory there terminates (MemoryResource contract).
    [[nodiscard]] EntityId Create() noexcept;
    /// Invalidates the handle; the index is recycled with the next generation.
    void Destroy(EntityId entity) noexcept;
    [[nodiscard]] bool IsAlive(EntityId entity) const noexcept;

    [[nodiscard]] EntityLocation GetLocation(EntityId entity) const noexcept;
    void SetLocation(EntityId entity, EntityLocation location) noexcept;

    [[nodiscard]] uint32_t GetAliveCount() const noexcept { return mAliveCount; }
    /// Highest index ever handed out plus one; bounds any per-entity side table.
    [[nodiscard]] uint32_t GetCapacity() const noexcept { return static_cast<uint32_t>(mSlots.size()); }

    // --- the slot table itself, for World::Save / Load (ADR-0013) ------------------------------

    /// Generation of a slot (0 for an index never handed out) and whether it is alive now.
    [[nodiscard]] uint32_t GetGeneration(uint32_t index) const noexcept;
    [[nodiscard]] bool IsIndexAlive(uint32_t index) const noexcept;
    /// Replaces the table: one generation and alive flag per index. Locations start empty; the
    /// free list holds the dead indices with the lowest on top. Generations must not be zero.
    void Restore(std::span<const uint32_t> generations, std::span<const uint8_t> alive) noexcept;
    /// Forgets every entity and every index.
    void Clear() noexcept;

private:
    struct Slot {
        uint32_t mGeneration = 1;
        bool mAlive          = false;
        EntityLocation mLocation;
    };

    core::MemoryResource mResource;
    std::pmr::vector<Slot> mSlots;
    std::pmr::vector<uint32_t> mFreeIndices;
    uint32_t mAliveCount = 0;
};

} // namespace oyi::ecs
