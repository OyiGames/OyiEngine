#pragma once

#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ComponentMask.h>

#include <array>
#include <cstdint>

namespace oyi::ecs {

/// The component types one World knows about, each with its generated ComponentInfo (ADR-0011).
/// Registration happens once at World setup; the dense ComponentTypeIndex is what storage,
/// queries and access masks use afterwards.
class ComponentRegistry {
public:
    ComponentRegistry() = default;

    /// Registers a reflected type. Registering the same type again returns the existing index;
    /// a type with the same TypeId but a different layout fails with AlreadyExists, and a full
    /// registry fails with OutOfRange.
    [[nodiscard]] core::Expected<ComponentTypeIndex> Register(const core::ComponentInfo& info) noexcept;

    template <typename T>
    [[nodiscard]] core::Expected<ComponentTypeIndex> Register() noexcept {
        return Register(core::ComponentTraits<T>::kInfo);
    }

    /// Invalid when the type is unknown. Linear in the number of registered types: cache the
    /// result rather than calling it per entity.
    [[nodiscard]] ComponentTypeIndex Find(core::TypeId typeId) const noexcept;

    template <typename T>
    [[nodiscard]] ComponentTypeIndex Find() const noexcept {
        return Find(core::ComponentTraits<T>::kInfo.mTypeId);
    }

    [[nodiscard]] const core::ComponentInfo& GetInfo(ComponentTypeIndex type) const noexcept;
    [[nodiscard]] bool IsRegistered(ComponentTypeIndex type) const noexcept { return type.mValue < mCount; }
    [[nodiscard]] uint32_t GetCount() const noexcept { return mCount; }

private:
    std::array<const core::ComponentInfo*, kMaxComponentTypes> mInfos{};
    uint32_t mCount = 0;
};

} // namespace oyi::ecs
