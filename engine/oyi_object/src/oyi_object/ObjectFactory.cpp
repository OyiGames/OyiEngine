#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/Entity.h>
#include <oyi_object/Components.h>
#include <oyi_object/ObjectFactory.h>
#include <oyi_object/ObjectLayer.h>
#include <oyi_object/Prototype.h>

#include <cstddef>
#include <cstdint>

namespace oyi::object {

ecs::PendingEntity ObjectFactory::Spawn(PrototypeId prototype, const SpawnOverrides& overrides) noexcept {
    if (!mLayer.GetPrototypes().IsRegistered(prototype)) {
        return ecs::PendingEntity{};
    }
    return SpawnNode(mLayer.GetPrototypes().Get(prototype), ecs::PendingEntity{}, ecs::PendingEntity{}, &overrides);
}

void ObjectFactory::Despawn(ecs::EntityId entity) noexcept {
    mCommands.Call(entity, &ObjectLayer::OnDespawnReplayed, &mLayer, nullptr, 0);
}

// NOLINTNEXTLINE(misc-no-recursion)
ecs::PendingEntity ObjectFactory::SpawnNode(const Prototype& prototype, ecs::PendingEntity root,
                                            ecs::PendingEntity parent, const SpawnOverrides* overrides) noexcept {
    const ecs::PendingEntity pending = mCommands.CloneEntity(prototype.mEntity);
    if (overrides != nullptr) {
        for (const ComponentValue& value : overrides->mValues) {
            if (prototype.mMask.Test(value.mType)) {
                mCommands.SetComponent(pending, value.mType, value.mData, value.mSize);
            }
        }
    }
    const ecs::PendingEntity objectRoot = root.IsValid() ? root : pending;
    mCommands.LinkEntity(pending, mLayer.GetTypes().mObjectInfo, static_cast<uint32_t>(offsetof(ObjectInfo, mRoot)),
                         objectRoot);
    const ParentLink& link = mLayer.GetParentLink();
    if (parent.IsValid() && link.mType.IsValid() && prototype.mMask.Test(link.mType)) {
        mCommands.LinkEntity(pending, link.mType, link.mOffset, parent);
    }
    mCommands.Call(pending, &ObjectLayer::OnSpawnReplayed, &mLayer, &prototype.mId,
                   static_cast<uint32_t>(sizeof(PrototypeId)));
    for (const PrototypeId child : prototype.mChildren) {
        (void)SpawnNode(mLayer.GetPrototypes().Get(child), objectRoot, pending, nullptr);
    }
    return pending;
}

} // namespace oyi::object
