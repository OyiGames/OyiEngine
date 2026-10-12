#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/Resource.h>

#include <cstdint>
#include <string_view>

namespace oyi::ecs {

core::Expected<ResourceTypeIndex> ResourceRegistry::Register(std::string_view name, void* instance,
                                                             ResourceOrdering ordering) noexcept {
    if (name.empty() || instance == nullptr) {
        return core::Fail(core::ErrorCode::InvalidArgument,
                          "ResourceRegistry: a resource needs a name and an instance");
    }
    const core::TypeId typeId = core::MakeTypeId(name);
    if (Find(typeId).IsValid()) {
        return core::Fail(core::ErrorCode::AlreadyExists, "ResourceRegistry: the name is already registered");
    }
    if (mCount >= kMaxResourceTypes) {
        return core::Fail(core::ErrorCode::OutOfRange, "ResourceRegistry: too many resources");
    }
    const ResourceTypeIndex index{.mValue = static_cast<uint16_t>(mCount)};
    mInfos[mCount] = ResourceInfo{.mName = name, .mTypeId = typeId, .mOrdering = ordering, .mInstance = instance};
    if (ordering == ResourceOrdering::Automatic) {
        mAutomatic.Set(index);
    }
    ++mCount;
    return index;
}

ResourceTypeIndex ResourceRegistry::Find(core::TypeId typeId) const noexcept {
    for (uint32_t i = 0; i < mCount; ++i) {
        if (mInfos[i].mTypeId == typeId) {
            return ResourceTypeIndex{.mValue = static_cast<uint16_t>(i)};
        }
    }
    return ResourceTypeIndex::Invalid();
}

const ResourceInfo& ResourceRegistry::GetInfo(ResourceTypeIndex type) const noexcept {
    OYI_ASSERT(IsRegistered(type), "ResourceRegistry: unknown resource");
    return mInfos[type.mValue];
}

} // namespace oyi::ecs
