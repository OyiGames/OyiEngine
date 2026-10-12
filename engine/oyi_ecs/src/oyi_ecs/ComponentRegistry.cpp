#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>

#include <cstdint>

namespace oyi::ecs {

core::Expected<ComponentTypeIndex> ComponentRegistry::Register(const core::ComponentInfo& info) noexcept {
    if (!info.mTypeId.IsValid()) {
        return core::Fail(core::ErrorCode::InvalidArgument, "ComponentRegistry: component has no TypeId");
    }
    const ComponentTypeIndex existing = Find(info.mTypeId);
    if (existing.IsValid()) {
        if (mInfos[existing.mValue]->mLayoutHash != info.mLayoutHash) {
            return core::Fail(core::ErrorCode::AlreadyExists,
                              "ComponentRegistry: a type with this TypeId but another layout is registered");
        }
        return existing;
    }
    if (mCount >= kMaxComponentTypes) {
        return core::Fail(core::ErrorCode::OutOfRange, "ComponentRegistry: too many component types");
    }
    const ComponentTypeIndex index{.mValue = static_cast<uint16_t>(mCount)};
    mInfos[mCount] = &info;
    ++mCount;
    return index;
}

ComponentTypeIndex ComponentRegistry::Find(core::TypeId typeId) const noexcept {
    for (uint32_t i = 0; i < mCount; ++i) {
        if (mInfos[i]->mTypeId == typeId) {
            return ComponentTypeIndex{.mValue = static_cast<uint16_t>(i)};
        }
    }
    return ComponentTypeIndex::Invalid();
}

const core::ComponentInfo& ComponentRegistry::GetInfo(ComponentTypeIndex type) const noexcept {
    OYI_ASSERT(IsRegistered(type), "ComponentRegistry: unknown component type");
    return *mInfos[type.mValue];
}

} // namespace oyi::ecs
