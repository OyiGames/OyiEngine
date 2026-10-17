#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/World.h>
#include <oyi_object/Components.h>

#include <cstdint>
#include <memory_resource> // IWYU pragma: keep (libc++ sees no use, the MSVC STL needs it)
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oyi::object {

/// One component's bytes: a prototype default or a spawn override. `mSize` must be the
/// component's size.
struct ComponentValue {
    ecs::ComponentTypeIndex mType;
    const void* mData = nullptr;
    uint32_t mSize    = 0;
};

template <typename T>
[[nodiscard]] ComponentValue ValueOf(ecs::ComponentTypeIndex type, const T& value) noexcept {
    return ComponentValue{.mType = type, .mData = &value, .mSize = static_cast<uint32_t>(sizeof(T))};
}

/// A managed type a prototype instantiates and the chunk-side component that holds its
/// ManagedId (at `mOffset` bytes into the component; 0 for a ManagedReference-shaped one).
struct ManagedBinding {
    const ecs::ManagedTypeInfo* mType = nullptr;
    ecs::ComponentTypeIndex mReference;
    uint32_t mOffset = 0;
};

/// What a prototype is (ADR-0024): a component set, default values, managed types and children.
/// Registered from code now; blobs come with the prototype asset (RFC-0001 step 6).
struct PrototypeDesc {
    std::string_view mName;
    ecs::ComponentMask mMask; ///< ObjectInfo is added by the registry
    std::span<const ComponentValue> mDefaults;
    std::span<const ManagedBinding> mManaged;
    std::span<const PrototypeDesc* const> mChildren;
};

/// A registered prototype: its entity (a prototype entity, outside every query) and what the
/// factory needs to clone it.
struct Prototype {
    std::pmr::string mName;
    PrototypeId mId;
    PrototypeId mParent; ///< invalid for a root prototype
    ecs::EntityId mEntity;
    ecs::ComponentMask mMask;
    std::pmr::vector<ManagedBinding> mManaged;
    std::pmr::vector<PrototypeId> mChildren;
};

/// The prototypes of one World. Registration instantiates each description (and, recursively,
/// its children) as a prototype entity with the defaults applied; the factory clones it.
class PrototypeRegistry {
public:
    PrototypeRegistry(core::Allocator& allocator, ecs::World& world, ecs::ComponentTypeIndex objectInfo) noexcept;
    PrototypeRegistry(const PrototypeRegistry&)            = delete;
    PrototypeRegistry(PrototypeRegistry&&)                 = delete;
    PrototypeRegistry& operator=(const PrototypeRegistry&) = delete;
    PrototypeRegistry& operator=(PrototypeRegistry&&)      = delete;
    ~PrototypeRegistry();

    /// Fails with AlreadyExists for a name in use, InvalidArgument for a default or a managed
    /// binding that does not fit the mask, and with the world's error when it cannot create the
    /// prototype entity. Registers every managed type with the world's managed type registry.
    [[nodiscard]] core::Expected<PrototypeId> Register(const PrototypeDesc& desc) noexcept;

    [[nodiscard]] PrototypeId Find(std::string_view name) const noexcept;
    [[nodiscard]] bool IsRegistered(PrototypeId id) const noexcept {
        return id.IsValid() && id.mIndex < mPrototypes.size() && id.mGeneration == 1;
    }
    [[nodiscard]] const Prototype& Get(PrototypeId id) const noexcept { return *mPrototypes[id.mIndex]; }
    [[nodiscard]] uint32_t GetCount() const noexcept { return static_cast<uint32_t>(mPrototypes.size()); }

private:
    [[nodiscard]] core::Expected<PrototypeId> RegisterNode(const PrototypeDesc& desc, PrototypeId parent) noexcept;

    core::Allocator& mAllocator;
    ecs::World& mWorld;
    ecs::ComponentTypeIndex mObjectInfo;
    core::MemoryResource mResource;
    std::pmr::vector<Prototype*> mPrototypes;
};

} // namespace oyi::object
