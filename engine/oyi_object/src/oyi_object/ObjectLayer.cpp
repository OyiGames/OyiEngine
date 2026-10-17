#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/World.h>
#include <oyi_object/Components.gen.h> // IWYU pragma: keep
#include <oyi_object/Components.h>
#include <oyi_object/ObjectLayer.h>
#include <oyi_object/Prototype.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace oyi::object {

core::Expected<ObjectTypes> RegisterObjectTypes(ecs::World& world) noexcept {
    const core::Expected<ecs::ComponentTypeIndex> info     = world.GetComponents().Register<ObjectInfo>();
    const core::Expected<ecs::MessageTypeIndex> spawned    = world.GetMessages().Register<Spawned>();
    const core::Expected<ecs::MessageTypeIndex> despawning = world.GetMessages().Register<Despawning>();
    if (!info.has_value()) {
        return core::Fail(info.error().mCode, info.error().mMessage);
    }
    if (!spawned.has_value()) {
        return core::Fail(spawned.error().mCode, spawned.error().mMessage);
    }
    if (!despawning.has_value()) {
        return core::Fail(despawning.error().mCode, despawning.error().mMessage);
    }
    return ObjectTypes{.mObjectInfo = *info, .mSpawned = *spawned, .mDespawning = *despawning};
}

ObjectLayer::ObjectLayer(core::Allocator& allocator, ecs::World& world, const ObjectTypes& types) noexcept
    : mWorld(world)
    , mTypes(types)
    , mResource(allocator)
    , mPrototypes(allocator, world, types.mObjectInfo)
    , mSpawned(&mResource)
    , mDespawnRequests(&mResource)
    , mDying(&mResource)
    , mBatch(&mResource)
    , mLeaving(&mResource)
    , mObjects(&mResource) {}

core::Result ObjectLayer::Install(ecs::Schedule& schedule) noexcept {
    const core::Expected<ecs::ResourceTypeIndex> resource =
        mWorld.GetResources().Register("oyi::object::ObjectLayer", *this, ecs::ResourceOrdering::Explicit);
    if (!resource.has_value()) {
        return core::Fail(resource.error().mCode, resource.error().mMessage);
    }
    mResourceType = *resource;
    schedule.SetSyncHook(&ObjectLayer::SyncHook, this);
    return {};
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void ObjectLayer::OnSpawnReplayed(ecs::World& /*world*/, ecs::EntityId entity, void* data,
                                  std::span<const std::byte> payload) {
    auto* const self = static_cast<ObjectLayer*>(data);
    PrototypeId prototype;
    if (payload.size() == sizeof(prototype)) {
        std::memcpy(&prototype, payload.data(), sizeof(prototype));
    }
    self->mSpawned.push_back(SpawnRecord{.mEntity = entity, .mPrototype = prototype});
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void ObjectLayer::OnDespawnReplayed(ecs::World& /*world*/, ecs::EntityId entity, void* data,
                                    std::span<const std::byte> /*payload*/) {
    auto* const self = static_cast<ObjectLayer*>(data);
    if (std::ranges::find(self->mDespawnRequests, entity) == self->mDespawnRequests.end()) {
        self->mDespawnRequests.push_back(entity);
    }
}

void ObjectLayer::SyncHook(ecs::FirewallWindow& window) {
    auto* const self = window.GetData<ObjectLayer>();
    self->mLastHook  = HookStats{};
    self->DestroyDying();
    self->FinishSpawns(window);
    self->BeginDespawns(window);
}

/// Despawning went out at the previous hook: now the objects go, then the entity.
// NOLINTNEXTLINE(bugprone-exception-escape)
void ObjectLayer::DestroyDying() noexcept {
    if (mDying.empty()) {
        return;
    }
    mObjects.clear();
    mWorld.GetManaged().ForEach([this](const ecs::ManagedObject& object) {
        if (std::ranges::find(mDying, object.GetOwner()) != mDying.end()) {
            mObjects.push_back(object.GetId());
        }
    });
    for (const ecs::ManagedId id : mObjects) {
        mWorld.GetManaged().Destroy(id);
    }
    for (const ecs::EntityId entity : mDying) {
        if (mWorld.DestroyEntity(entity).has_value()) {
            ++mLastHook.mDestroyed;
        }
    }
    mDying.clear();
}

/// Every entity the last replay materialised: objects first (the whole batch exists before any
/// OnSpawned runs), then OnSpawned in replay order, then Spawned.
// NOLINTNEXTLINE(bugprone-exception-escape)
void ObjectLayer::FinishSpawns(ecs::FirewallWindow& window) noexcept {
    mBatch.clear();
    if (mSpawned.empty()) {
        return;
    }
    mBatch.swap(mSpawned);
    mObjects.clear();
    for (const SpawnRecord& record : mBatch) {
        if (!mWorld.IsAlive(record.mEntity) || !mPrototypes.IsRegistered(record.mPrototype)) {
            continue;
        }
        for (const ManagedBinding& binding : mPrototypes.Get(record.mPrototype).mManaged) {
            void* const reference = mWorld.GetComponentForWrite(record.mEntity, binding.mReference);
            if (reference == nullptr) {
                continue; // the clone lost the component: not a factory-made row any more
            }
            ecs::ManagedObject* const object =
                binding.mType->mCreate(mWorld.GetManaged().GetAllocator(), *binding.mType);
            if (object == nullptr) {
                continue;
            }
            const core::Expected<ecs::ManagedId> id = mWorld.GetManaged().Adopt(object, record.mEntity);
            if (!id.has_value()) {
                mWorld.GetManaged().GetAllocator().Delete(object);
                continue;
            }
            std::memcpy(static_cast<std::byte*>(reference) + binding.mOffset, &*id, sizeof(ecs::ManagedId));
            mObjects.push_back(*id);
        }
    }
    for (const ecs::ManagedId id : mObjects) {
        if (ecs::ManagedObject* const object = mWorld.GetManaged().Resolve(id); object != nullptr) {
            object->OnSpawned(window);
        }
    }
    for (const SpawnRecord& record : mBatch) {
        if (mWorld.IsAlive(record.mEntity)) {
            window.Send(mTypes.mSpawned, record.mEntity, Spawned{.mPrototype = record.mPrototype});
            ++mLastHook.mSpawned;
        }
    }
}

/// Despawn requests whose entity exists: the entity and every entity rooted at it get
/// OnDespawning and Despawning now and die at the next hook. A request for an entity that was
/// spawned at this very sync point waits for the next hook.
// NOLINTNEXTLINE(bugprone-exception-escape)
void ObjectLayer::BeginDespawns(ecs::FirewallWindow& window) noexcept {
    if (mDespawnRequests.empty()) {
        mBatch.clear();
        return;
    }
    mLeaving.clear();
    const auto spawnedNow = [this](ecs::EntityId entity) {
        return std::ranges::any_of(mBatch, [entity](const SpawnRecord& record) { return record.mEntity == entity; });
    };
    const auto leaving = [this](ecs::EntityId entity) { return std::ranges::find(mLeaving, entity) != mLeaving.end(); };
    uint32_t kept      = 0;
    for (const ecs::EntityId root : mDespawnRequests) {
        if (!mWorld.IsAlive(root) || leaving(root)) {
            continue;
        }
        if (spawnedNow(root)) {
            mDespawnRequests[kept++] = root;
            continue;
        }
        mLeaving.push_back(root);
    }
    mDespawnRequests.resize(kept);
    mBatch.clear();
    if (mLeaving.empty()) {
        return;
    }
    // The rest of each object: every entity whose root is a leaving entity.
    const size_t rootCount = mLeaving.size();
    ecs::ComponentMask objects;
    objects.Set(mTypes.mObjectInfo);
    mWorld.ForEachChunk(objects, ecs::ComponentMask{}, [&](ecs::ChunkView& view) {
        const std::span<const ecs::EntityId> entities = view.GetEntities();
        const std::span<const ObjectInfo> infos       = view.Read<ObjectInfo>(mTypes.mObjectInfo);
        for (size_t i = 0; i < entities.size(); ++i) {
            const auto root = std::ranges::find(mLeaving.begin(), mLeaving.begin() + static_cast<ptrdiff_t>(rootCount),
                                                infos[i].mRoot);
            if (root != mLeaving.begin() + static_cast<ptrdiff_t>(rootCount) && !leaving(entities[i])) {
                mLeaving.push_back(entities[i]);
            }
        }
    });
    mObjects.clear();
    mWorld.GetManaged().ForEach([this, &leaving](const ecs::ManagedObject& object) {
        if (leaving(object.GetOwner())) {
            mObjects.push_back(object.GetId());
        }
    });
    for (const ecs::ManagedId id : mObjects) {
        if (ecs::ManagedObject* const object = mWorld.GetManaged().Resolve(id); object != nullptr) {
            object->OnDespawning(window);
        }
    }
    for (const ecs::EntityId entity : mLeaving) {
        const auto* const info = mWorld.Get<ObjectInfo>(entity);
        window.Send(mTypes.mDespawning, entity,
                    Despawning{.mPrototype = info != nullptr ? info->mPrototype : PrototypeId::Invalid()});
        mDying.push_back(entity);
        ++mLastHook.mDespawning;
    }
}

} // namespace oyi::object
