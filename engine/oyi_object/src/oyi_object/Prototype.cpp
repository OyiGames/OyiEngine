#include <oyi_asset/Blob.h>
#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/World.h>
#include <oyi_object/Components.gen.h> // IWYU pragma: keep
#include <oyi_object/Components.h>
#include <oyi_object/Prototype.h>
#include <oyi_object/PrototypeBlob.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory_resource> // IWYU pragma: keep (libc++ sees no use, the MSVC STL needs it)
#include <span>
#include <string_view>
#include <vector>

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
    const core::Expected<PrototypeId> id = RegisterOne(desc, parent);
    if (!id.has_value()) {
        return id;
    }
    for (const PrototypeDesc* const child : desc.mChildren) {
        const core::Expected<PrototypeId> childId = RegisterNode(*child, *id);
        if (!childId.has_value()) {
            return childId;
        }
        mPrototypes[id->mIndex]->mChildren.push_back(*childId);
    }
    return id;
}

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<PrototypeId> PrototypeRegistry::RegisterOne(const PrototypeDesc& desc, PrototypeId parent) noexcept {
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
    return id;
}

namespace {

/// A span of the blob is only trusted when it lies inside the payload.
template <typename T>
[[nodiscard]] bool InPayload(const asset::BlobView& blob, std::span<const T> span) noexcept {
    const std::span<const std::byte> payload = blob.GetPayload();
    const auto* const begin                  = reinterpret_cast<const std::byte*>(span.data()); // NOLINT
    return span.empty() || (begin >= payload.data() &&
                            span.size_bytes() <= static_cast<size_t>(payload.data() + payload.size() - begin));
}

} // namespace

// NOLINTNEXTLINE(bugprone-exception-escape)
core::Expected<PrototypeId> PrototypeRegistry::Load(const asset::BlobView& blob) noexcept {
    const std::span<const PrototypeNode> nodes = blob.Root<PrototypeBlob>()->mNodes.Get();
    if (nodes.empty() || !InPayload(blob, nodes)) {
        return core::Fail(core::ErrorCode::CorruptData, "PrototypeRegistry::Load: the blob holds no prototype tree");
    }
    std::pmr::vector<uint8_t> visited(nodes.size(), 0, &mResource);
    return LoadNode(blob, 0, PrototypeId::Invalid(), visited);
}

// NOLINTNEXTLINE(bugprone-exception-escape,misc-no-recursion,readability-function-cognitive-complexity)
core::Expected<PrototypeId> PrototypeRegistry::LoadNode(const asset::BlobView& blob, uint32_t index, PrototypeId parent,
                                                        std::pmr::vector<uint8_t>& visited) noexcept {
    const std::span<const PrototypeNode> nodes = blob.Root<PrototypeBlob>()->mNodes.Get();
    if (index >= nodes.size() || visited[index] != 0) {
        return core::Fail(core::ErrorCode::CorruptData, "PrototypeRegistry::Load: the prototype tree is not a tree");
    }
    visited[index]                                        = 1;
    const PrototypeNode& node                             = nodes[index];
    const std::span<const char> name                      = node.mName.Get();
    const std::span<const uint64_t> components            = node.mComponents.Get();
    const std::span<const PrototypeValueRecord> defaults  = node.mDefaults.Get();
    const std::span<const PrototypeManagedRecord> managed = node.mManaged.Get();
    const std::span<const uint32_t> children              = node.mChildren.Get();
    if (!InPayload(blob, name) || !InPayload(blob, components) || !InPayload(blob, defaults) ||
        !InPayload(blob, managed) || !InPayload(blob, children)) {
        return core::Fail(core::ErrorCode::CorruptData, "PrototypeRegistry::Load: a prototype points outside the blob");
    }
    ecs::ComponentMask mask;
    for (const uint64_t typeId : components) {
        const ecs::ComponentTypeIndex type = mWorld.GetComponents().Find(core::TypeId{.mValue = typeId});
        if (!type.IsValid()) {
            return core::Fail(core::ErrorCode::NotFound,
                              "PrototypeRegistry::Load: the world lacks a component of the prototype");
        }
        mask.Set(type);
    }
    std::pmr::vector<ComponentValue> values(&mResource);
    for (const PrototypeValueRecord& record : defaults) {
        const ecs::ComponentTypeIndex type     = mWorld.GetComponents().Find(core::TypeId{.mValue = record.mTypeId});
        const std::span<const std::byte> bytes = record.mBytes.Get();
        if (!type.IsValid() || !InPayload(blob, bytes)) {
            return core::Fail(core::ErrorCode::NotFound,
                              "PrototypeRegistry::Load: the world lacks a component of a default");
        }
        values.push_back(
            ComponentValue{.mType = type, .mData = bytes.data(), .mSize = static_cast<uint32_t>(bytes.size())});
    }
    std::pmr::vector<ManagedBinding> bindings(&mResource);
    for (const PrototypeManagedRecord& record : managed) {
        const ecs::ManagedTypeInfo* const info =
            mWorld.GetManagedTypes().Find(core::TypeId{.mValue = record.mStateTypeId});
        const ecs::ComponentTypeIndex reference =
            mWorld.GetComponents().Find(core::TypeId{.mValue = record.mReferenceTypeId});
        if (info == nullptr || !reference.IsValid()) {
            return core::Fail(
                core::ErrorCode::NotFound,
                "PrototypeRegistry::Load: the world lacks a managed type of the prototype (register it first)");
        }
        bindings.push_back(ManagedBinding{.mType = info, .mReference = reference, .mOffset = record.mOffset});
    }
    const PrototypeDesc desc{.mName     = std::string_view(name.data(), name.size()),
                             .mMask     = mask,
                             .mDefaults = values,
                             .mManaged  = bindings,
                             .mChildren = {}};
    const core::Expected<PrototypeId> id = RegisterOne(desc, parent);
    if (!id.has_value()) {
        return id;
    }
    for (const uint32_t child : children) {
        const core::Expected<PrototypeId> childId = LoadNode(blob, child, *id, visited);
        if (!childId.has_value()) {
            return childId;
        }
        mPrototypes[id->mIndex]->mChildren.push_back(*childId);
    }
    return id;
}

} // namespace oyi::object
