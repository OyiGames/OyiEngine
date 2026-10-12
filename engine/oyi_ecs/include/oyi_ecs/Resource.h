#pragma once

#include <oyi_core/Expected.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/ComponentMask.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace oyi::ecs {

/// Resources (ADR-0007: `Resource<T>`): data that is not a component but that systems read and
/// write, such as a physics world, a navigation mesh or a render queue. A resource is any object
/// the owner keeps alive and registers with the World by name; systems declare which resources
/// they read and write exactly like components, and the schedule applies the same conflict rules.
/// Resources are opaque to the kernel: no layout, no snapshot, no command buffer.

inline constexpr uint32_t kMaxResourceTypes = 64;

struct ResourceTypeIndex {
    uint16_t mValue = 0xFFFFu;

    [[nodiscard]] static constexpr ResourceTypeIndex Invalid() noexcept { return ResourceTypeIndex{}; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return mValue < kMaxResourceTypes; }

    [[nodiscard]] constexpr auto operator<=>(const ResourceTypeIndex&) const noexcept = default;
};

/// Which resources a system reads or writes.
using ResourceMask = TypeMask<ResourceTypeIndex>;

/// How the schedule treats two systems that conflict on the resource.
enum class ResourceOrdering : uint8_t {
    /// Like components: an unordered conflicting pair fails Build (ADR-0007).
    Explicit,
    /// The schedule orders conflicting pairs by registration order on its own (ADR-0007: writers
    /// of the Environment serialize automatically). For resources that are written rarely and
    /// read everywhere; the order is still deterministic and visible through GetPredecessors.
    Automatic,
};

struct ResourceInfo {
    std::string_view mName;
    core::TypeId mTypeId;
    ResourceOrdering mOrdering = ResourceOrdering::Explicit;
    void* mInstance            = nullptr;
};

/// The resources one World knows about, by name. Registration happens at World setup; the dense
/// ResourceTypeIndex is what access masks and contexts use afterwards.
class ResourceRegistry {
public:
    ResourceRegistry() = default;

    /// `name` is the fully qualified type name (its hash is the TypeId); `instance` must outlive
    /// the World. Fails with InvalidArgument for an empty name or null instance, AlreadyExists for
    /// a name that is registered, OutOfRange when full.
    [[nodiscard]] core::Expected<ResourceTypeIndex> Register(std::string_view name, void* instance,
                                                             ResourceOrdering ordering) noexcept;

    template <typename T>
    [[nodiscard]] core::Expected<ResourceTypeIndex>
    Register(std::string_view name, T& instance, ResourceOrdering ordering = ResourceOrdering::Explicit) noexcept {
        return Register(name, static_cast<void*>(&instance), ordering);
    }

    /// Invalid when unknown. Linear: cache the result.
    [[nodiscard]] ResourceTypeIndex Find(core::TypeId typeId) const noexcept;
    [[nodiscard]] ResourceTypeIndex Find(std::string_view name) const noexcept { return Find(core::MakeTypeId(name)); }

    [[nodiscard]] const ResourceInfo& GetInfo(ResourceTypeIndex type) const noexcept;
    [[nodiscard]] bool IsRegistered(ResourceTypeIndex type) const noexcept { return type.mValue < mCount; }
    [[nodiscard]] uint32_t GetCount() const noexcept { return mCount; }
    /// The resources registered with ResourceOrdering::Automatic.
    [[nodiscard]] const ResourceMask& GetAutomaticMask() const noexcept { return mAutomatic; }

    /// The registered instance; nullptr for an unknown index. The caller names the type.
    [[nodiscard]] void* Get(ResourceTypeIndex type) const noexcept {
        return IsRegistered(type) ? mInfos[type.mValue].mInstance : nullptr;
    }

    template <typename T>
    [[nodiscard]] T* Get(ResourceTypeIndex type) const noexcept {
        return static_cast<T*>(Get(type));
    }

private:
    std::array<ResourceInfo, kMaxResourceTypes> mInfos{};
    ResourceMask mAutomatic;
    uint32_t mCount = 0;
};

/// The per-world singleton every World registers as its first resource (ADR-0007), carrying the
/// time model of ADR-0014. The main loop writes it between phases; systems that write it inside a
/// phase are ordered automatically.
struct Environment {
    uint64_t mFrameIndex               = 0; ///< frames the owner has run
    uint64_t mSimulationTick           = 0; ///< integer fixed-step ticks; server-authoritative, never scaled
    float mFixedStepSeconds            = 1.0f / 30.0f;
    double mPresentationSeconds        = 0.0;  ///< variable-rate clock for interpolation, animation, effects
    float mDeltaSeconds                = 0.0f; ///< presentation time elapsed since the previous frame
    int64_t mPersistentUtcMilliseconds = 0;    ///< wall clock for lazy settlement of offline progress
};

inline constexpr std::string_view kEnvironmentResourceName = "oyi::ecs::Environment";
/// Always index 0: the World registers the Environment before anything else.
inline constexpr ResourceTypeIndex kEnvironmentResource{.mValue = 0};

} // namespace oyi::ecs
