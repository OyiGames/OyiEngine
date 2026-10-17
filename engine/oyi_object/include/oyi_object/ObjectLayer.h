#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/World.h>
#include <oyi_object/Components.h>
#include <oyi_object/Prototype.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource> // IWYU pragma: keep (libc++ sees no use, the MSVC STL needs it)
#include <span>
#include <vector>

namespace oyi::object {

/// The object layer's registrations in one World.
struct ObjectTypes {
    ecs::ComponentTypeIndex mObjectInfo;
    ecs::MessageTypeIndex mSpawned;
    ecs::MessageTypeIndex mDespawning;
};

[[nodiscard]] core::Expected<ObjectTypes> RegisterObjectTypes(ecs::World& world) noexcept;

/// Where a child's parent id goes at spawn: a handle field of a component in the child's mask
/// (transform's Parent, for one). Children without that component get no link.
struct ParentLink {
    ecs::ComponentTypeIndex mType;
    uint32_t mOffset = 0;
};

/// The object layer of one World (ADR-0024): the prototypes, and the sync hook that finishes
/// what the factory's commands began. Spawn: the clone and its overrides replay like any command;
/// in the hook that follows, every entity spawned at that sync point gets its managed objects,
/// they run OnSpawned with the whole batch visible, then each entity receives Spawned. Despawn:
/// in the hook, OnDespawning runs and Despawning goes out while the entity is alive; the hook of
/// the next sync point destroys the managed objects and the entity. Both follow replay order.
class ObjectLayer {
public:
    ObjectLayer(core::Allocator& allocator, ecs::World& world, const ObjectTypes& types) noexcept;
    ObjectLayer(const ObjectLayer&)            = delete;
    ObjectLayer(ObjectLayer&&)                 = delete;
    ObjectLayer& operator=(const ObjectLayer&) = delete;
    ObjectLayer& operator=(ObjectLayer&&)      = delete;
    ~ObjectLayer()                             = default;

    /// Binds the layer to the schedule's sync hook and registers it as the world resource
    /// "oyi::object::ObjectLayer" (systems that spawn declare it read). Fails if another
    /// resource holds the name.
    [[nodiscard]] core::Result Install(ecs::Schedule& schedule) noexcept;

    [[nodiscard]] PrototypeRegistry& GetPrototypes() noexcept { return mPrototypes; }
    [[nodiscard]] const PrototypeRegistry& GetPrototypes() const noexcept { return mPrototypes; }
    [[nodiscard]] const ObjectTypes& GetTypes() const noexcept { return mTypes; }
    [[nodiscard]] ecs::World& GetWorld() const noexcept { return mWorld; }
    [[nodiscard]] ecs::ResourceTypeIndex GetResourceType() const noexcept { return mResourceType; }

    void SetParentLink(ParentLink link) noexcept { mParentLink = link; }
    [[nodiscard]] const ParentLink& GetParentLink() const noexcept { return mParentLink; }

    /// What the last hook run did.
    struct HookStats {
        uint32_t mSpawned    = 0; ///< entities that got their objects and Spawned
        uint32_t mDespawning = 0; ///< entities that received Despawning
        uint32_t mDestroyed  = 0; ///< entities destroyed (Despawning went out one sync point earlier)
    };
    [[nodiscard]] HookStats GetLastHookStats() const noexcept { return mLastHook; }

    /// The factory's replay callbacks (CommandBuffer::Call): they only take notes for the hook.
    static void OnSpawnReplayed(ecs::World& world, ecs::EntityId entity, void* data,
                                std::span<const std::byte> payload);
    static void OnDespawnReplayed(ecs::World& world, ecs::EntityId entity, void* data,
                                  std::span<const std::byte> payload);
    /// The sync hook itself (Schedule::SetSyncHook); `window.GetData()` is the layer.
    static void SyncHook(ecs::FirewallWindow& window);

private:
    struct SpawnRecord {
        ecs::EntityId mEntity;
        PrototypeId mPrototype;
    };

    void DestroyDying() noexcept;
    void FinishSpawns(ecs::FirewallWindow& window) noexcept;
    void BeginDespawns(ecs::FirewallWindow& window) noexcept;

    ecs::World& mWorld;
    ObjectTypes mTypes;
    core::MemoryResource mResource;
    PrototypeRegistry mPrototypes;
    ParentLink mParentLink{};
    ecs::ResourceTypeIndex mResourceType;
    std::pmr::vector<SpawnRecord> mSpawned;           ///< materialised at the last replay, hook pending
    std::pmr::vector<ecs::EntityId> mDespawnRequests; ///< Despawn commands replayed, hook pending
    std::pmr::vector<ecs::EntityId> mDying;           ///< Despawning sent, destroyed at the next hook
    std::pmr::vector<SpawnRecord> mBatch;             ///< scratch: this hook's spawns
    std::pmr::vector<ecs::EntityId> mLeaving;         ///< scratch: this hook's despawns
    std::pmr::vector<ecs::ManagedId> mObjects;        ///< scratch: objects of a batch
    HookStats mLastHook;
};

} // namespace oyi::object
