#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>

#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::transform {

/// The transform component types as registered in one World.
struct TransformTypes {
    ecs::ComponentTypeIndex mTranslation;
    ecs::ComponentTypeIndex mRotation;
    ecs::ComponentTypeIndex mScale;
    ecs::ComponentTypeIndex mLocalToWorld;
    ecs::ComponentTypeIndex mLocalToParent;
    ecs::ComponentTypeIndex mParent;
    ecs::ComponentTypeIndex mPreviousParent;
    ecs::ComponentTypeIndex mManualTransform;
};

/// Registers every transform component with `world`.
[[nodiscard]] core::Expected<TransformTypes> RegisterTransformComponents(ecs::World& world) noexcept;

/// Per-world hierarchy state for the propagation system (ADR-0010): the parented entities as a
/// flat array sorted by depth, parents first, rebuilt only when the topology changed.
class TransformHierarchy {
public:
    TransformHierarchy(core::Allocator& allocator, TransformTypes types) noexcept;
    TransformHierarchy(const TransformHierarchy&)            = delete;
    TransformHierarchy(TransformHierarchy&&)                 = delete;
    TransformHierarchy& operator=(const TransformHierarchy&) = delete;
    TransformHierarchy& operator=(TransformHierarchy&&)      = delete;
    ~TransformHierarchy()                                    = default;

    [[nodiscard]] const TransformTypes& GetTypes() const noexcept { return mTypes; }
    /// Parented entities in propagation order.
    [[nodiscard]] std::span<const ecs::EntityId> GetOrder() const noexcept { return mOrder; }
    [[nodiscard]] uint32_t GetRebuildCount() const noexcept { return mRebuilds; }

    /// Called by the propagation system; public so that tests and tools can drive it.
    void Propagate(ecs::SystemContext& context) noexcept;

private:
    struct Entry {
        ecs::EntityId mEntity;
        uint32_t mDepth = 0;
    };

    [[nodiscard]] bool CollectAndCheck(ecs::SystemContext& context) noexcept;
    void Rebuild(ecs::SystemContext& context) noexcept;

    TransformTypes mTypes;
    core::MemoryResource mResource;
    std::pmr::vector<ecs::EntityId> mOrder;
    std::pmr::vector<Entry> mScratch;
    uint32_t mRebuilds = 0;
};

struct TransformSystemIds {
    ecs::SystemId mTrsToWorld;  ///< TRS -> LocalToWorld for entities without a Parent
    ecs::SystemId mTrsToParent; ///< TRS -> LocalToParent for entities with a Parent
    ecs::SystemId mPropagate;   ///< parent.LocalToWorld * LocalToParent in hierarchy order
    ecs::SystemId mOrphans;     ///< drops PreviousParent from entities that lost their Parent
};

/// Adds the transform systems to `schedule` (TransformPropagate phase) with their order edges.
/// `hierarchy` must outlive the schedule.
[[nodiscard]] core::Expected<TransformSystemIds> AddTransformSystems(ecs::Schedule& schedule,
                                                                     TransformHierarchy& hierarchy) noexcept;

} // namespace oyi::transform
