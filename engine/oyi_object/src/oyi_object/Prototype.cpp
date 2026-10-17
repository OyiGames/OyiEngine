#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/World.h>
#include <oyi_object/Components.gen.h> // IWYU pragma: keep
#include <oyi_object/Components.h>
#include <oyi_object/Prototype.h>

#include <cstdint>
#include <cstring>
#include <string_view>

namespace oyi::object {

PrototypeRegistry::PrototypeRegistry(core::Allocator& allocator, ecs::World& world,
                                     ecs::ComponentTypeIndex objectInfo) noexcept
    : mAllocator(allocator), mWorld(world), mObjectInfo(objectInfo), mResource(allocator), mPrototypes(&mResource) {}

PrototypeRegistry::~PrototypeRegistry() {
    for (Prototype* const prototype : mPrototypes) {
        mAllocator.Delete(prototype);
    }
}

core::Expected<PrototypeId> PrototypeRegistry::Register(const PrototypeDesc& desc) noexcept {
    return RegisterNode(desc, PrototypeId::Invalid());
}

PrototypeId PrototypeRegistry::Find(std::string_view name) const noexcept {
    for (const Prototype* const prototype : mPrototypes) {
        if (prototype->mName == name) {
            return prototype->mId;
        }
    }
    return PrototypeId::Invalid();
}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape,misc-no-recursion)
core::Expected<PrototypeId> PrototypeRegistry::RegisterNode(const PrototypeDesc& desc, PrototypeId parent) noexcept {
    if (desc.mName.empty()) {
        return core::Fail(core::ErrorCode::InvalidArgument, "PrototypeRegistry: a prototype needs a name");
    }
    if (Find(desc.mName).IsValid()) {
        return core::Fail(core::ErrorCode::AlreadyExists, "PrototypeRegistry: the name is taken");
    }
    ecs::ComponentMask mask = desc.mMask;
    mask.Set(mObjectInfo);
    for (const ComponentValue& value : desc.mDefaults) {
        if (!mask.Test(value.mType) || value.mData == nullptr ||
            mWorld.GetComponents().GetInfo(value.mType).mSize != value.mSize) {
            return core::Fail(core::ErrorCode::InvalidArgument, "PrototypeRegistry: a default does not fit the mask");
        }
    }
    for (const ManagedBinding& binding : desc.mManaged) {
        if (binding.mType == nullptr || !mask.Test(binding.mReference) ||
            static_cast<size_t>(binding.mOffset) + sizeof(ecs::ManagedId) >
                mWorld.GetComponents().GetInfo(binding.mReference).mSize) {
            return core::Fail(core::ErrorCode::InvalidArgument,
                              "PrototypeRegistry: a managed binding names no reference component in the mask");
        }
    }

    const core::Expected<ecs::EntityId> entity = mWorld.CreatePrototype(mask);
    if (!entity.has_value()) {
        return core::Fail(entity.error().mCode, entity.error().mMessage);
    }
    const PrototypeId id{.mIndex = static_cast<uint32_t>(mPrototypes.size()), .mGeneration = 1};
    for (const ComponentValue& value : desc.mDefaults) {
        std::memcpy(mWorld.GetComponentForWrite(*entity, value.mType), value.mData, value.mSize);
    }
    *mWorld.GetForWrite<ObjectInfo>(*entity) = ObjectInfo{.mPrototype = id, .mRoot = ecs::EntityId::Invalid()};
    for (const ManagedBinding& binding : desc.mManaged) {
        if (const core::Result registered = mWorld.GetManagedTypes().Register(*binding.mType);
            !registered.has_value()) {
            return core::Fail(registered.error().mCode, registered.error().mMessage);
        }
    }

    auto* const prototype =
        mAllocator.New<Prototype>(Prototype{.mName     = std::pmr::string(desc.mName, &mResource),
                                            .mId       = id,
                                            .mParent   = parent,
                                            .mEntity   = *entity,
                                            .mMask     = mask,
                                            .mManaged  = std::pmr::vector<ManagedBinding>(&mResource),
                                            .mChildren = std::pmr::vector<PrototypeId>(&mResource)});
    if (prototype == nullptr) {
        return core::Fail(core::ErrorCode::OutOfMemory, "PrototypeRegistry: cannot allocate the prototype");
    }
    prototype->mManaged.assign(desc.mManaged.begin(), desc.mManaged.end());
    mPrototypes.push_back(prototype);
    for (const PrototypeDesc* const child : desc.mChildren) {
        const core::Expected<PrototypeId> childId = RegisterNode(*child, id);
        if (!childId.has_value()) {
            return childId;
        }
        mPrototypes[id.mIndex]->mChildren.push_back(*childId);
    }
    return id;
}

} // namespace oyi::object
