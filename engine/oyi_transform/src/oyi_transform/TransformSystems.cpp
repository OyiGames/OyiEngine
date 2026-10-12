#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_transform/Affine.h>
#include <oyi_transform/Components.gen.h> // IWYU pragma: keep
#include <oyi_transform/Components.h>
#include <oyi_transform/TransformSystems.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oyi::transform {

namespace {

constexpr uint32_t kMaxDepth = 64; ///< deeper chains are treated as broken (a cycle or garbage)

[[nodiscard]] ecs::ComponentMask MaskOf(std::span<const ecs::ComponentTypeIndex> types) noexcept {
    ecs::ComponentMask mask;
    for (const ecs::ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

/// Composes TRS into `out` for every row of the chunk; Rotation and Scale are optional columns.
void ComposeChunk(const ecs::SystemContext& context, const ecs::ChunkView& view, const TransformTypes& types,
                  std::span<float> out) noexcept {
    const std::span<const Translation> translations = context.Read<Translation>(view, types.mTranslation);
    const std::span<const Rotation> rotations =
        view.Has(types.mRotation) ? context.Read<Rotation>(view, types.mRotation) : std::span<const Rotation>{};
    const std::span<const Scale> scales =
        view.Has(types.mScale) ? context.Read<Scale>(view, types.mScale) : std::span<const Scale>{};
    for (size_t i = 0; i < translations.size(); ++i) {
        const Rotation rotation = rotations.empty() ? Rotation{} : rotations[i];
        const Scale scale       = scales.empty() ? Scale{} : scales[i];
        Compose(translations[i], rotation, scale, AffineRef{out.subspan(i * 12, 12)});
    }
}

void TrsToLocalToWorld(ecs::SystemContext& context) {
    const auto* const hierarchy = context.GetData<TransformHierarchy>();
    const TransformTypes& types = hierarchy->GetTypes();
    context.ForEachChunk([&](ecs::ChunkView& view) {
        const std::span<LocalToWorld> matrices = context.Write<LocalToWorld>(view, types.mLocalToWorld);
        if (!matrices.empty()) {
            ComposeChunk(context, view, types, std::span<float>{matrices.data()->mM, matrices.size() * 12});
        }
    });
}

void TrsToLocalToParent(ecs::SystemContext& context) {
    const auto* const hierarchy = context.GetData<TransformHierarchy>();
    const TransformTypes& types = hierarchy->GetTypes();
    context.ForEachChunk([&](ecs::ChunkView& view) {
        const std::span<LocalToParent> matrices = context.Write<LocalToParent>(view, types.mLocalToParent);
        if (!matrices.empty()) {
            ComposeChunk(context, view, types, std::span<float>{matrices.data()->mM, matrices.size() * 12});
        }
    });
}

void PropagateSystem(ecs::SystemContext& context) {
    context.GetData<TransformHierarchy>()->Propagate(context);
}

void OrphanSystem(ecs::SystemContext& context) {
    const auto* const hierarchy = context.GetData<TransformHierarchy>();
    const TransformTypes& types = hierarchy->GetTypes();
    context.ForEachChunk([&](ecs::ChunkView& view) {
        for (const ecs::EntityId entity : view.GetEntities()) {
            context.GetCommands().RemoveComponent(entity, types.mPreviousParent);
        }
    });
}

} // namespace

core::Expected<TransformTypes> RegisterTransformComponents(ecs::World& world) noexcept {
    ecs::ComponentRegistry& registry = world.GetComponents();
    TransformTypes types;
    const auto take = [](core::Expected<ecs::ComponentTypeIndex> result, ecs::ComponentTypeIndex& slot) {
        if (result.has_value()) {
            slot = *result;
        }
        return result.has_value();
    };
    if (!take(registry.Register<Translation>(), types.mTranslation) ||
        !take(registry.Register<Rotation>(), types.mRotation) || !take(registry.Register<Scale>(), types.mScale) ||
        !take(registry.Register<LocalToWorld>(), types.mLocalToWorld) ||
        !take(registry.Register<LocalToParent>(), types.mLocalToParent) ||
        !take(registry.Register<Parent>(), types.mParent) ||
        !take(registry.Register<PreviousParent>(), types.mPreviousParent) ||
        !take(registry.Register<ManualTransform>(), types.mManualTransform)) {
        return core::Fail(core::ErrorCode::OutOfRange, "transform: component registry full");
    }
    return types;
}

// --- hierarchy --------------------------------------------------------------------------------

TransformHierarchy::TransformHierarchy(core::Allocator& allocator, TransformTypes types) noexcept
    : mTypes(types), mResource(allocator), mOrder(&mResource), mScratch(&mResource) {}

// pmr containers report exhaustion through MemoryResource, which terminates; they never throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
bool TransformHierarchy::CollectAndCheck(ecs::SystemContext& context) noexcept {
    // One pass over the parented entities: gather them, and compare Parent with PreviousParent to
    // find out whether the topology changed since the last rebuild (ADR-0010: no events).
    mScratch.clear();
    bool dirty = false;
    context.ForEachChunk([&](ecs::ChunkView& view) {
        const std::span<const Parent> parents    = context.Read<Parent>(view, mTypes.mParent);
        const std::span<const ecs::EntityId> ids = view.GetEntities();
        if (!view.Has(mTypes.mPreviousParent)) {
            dirty = true;
            for (size_t i = 0; i < ids.size(); ++i) {
                // System state is added by the system itself; the Set lands in the same replay.
                context.GetCommands().AddComponent(ids[i], mTypes.mPreviousParent);
                context.GetCommands().Set(ids[i], mTypes.mPreviousParent, PreviousParent{.mValue = parents[i].mValue});
            }
        } else {
            const std::span<PreviousParent> previous = context.Write<PreviousParent>(view, mTypes.mPreviousParent);
            for (size_t i = 0; i < ids.size(); ++i) {
                if (previous[i].mValue != parents[i].mValue) {
                    dirty              = true;
                    previous[i].mValue = parents[i].mValue;
                }
            }
        }
        for (const ecs::EntityId id : ids) {
            mScratch.push_back(Entry{.mEntity = id, .mDepth = 0});
        }
    });
    // Entities that left (destroyed, or lost their Parent) change the count without touching
    // anyone's PreviousParent.
    return dirty || mScratch.size() != mOrder.size();
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void TransformHierarchy::Rebuild(ecs::SystemContext& context) noexcept {
    for (Entry& entry : mScratch) {
        uint32_t depth        = 0;
        ecs::EntityId current = entry.mEntity;
        while (depth < kMaxDepth) {
            const auto* const parent = context.Lookup<Parent>(mTypes.mParent, current);
            if (parent == nullptr) {
                break;
            }
            ++depth;
            current = parent->mValue;
        }
        entry.mDepth = depth;
    }
    // Parents before children; the entity index breaks ties so the order is reproducible.
    std::ranges::sort(mScratch, [](const Entry& a, const Entry& b) {
        return a.mDepth != b.mDepth ? a.mDepth < b.mDepth : a.mEntity.mIndex < b.mEntity.mIndex;
    });
    mOrder.clear();
    for (const Entry& entry : mScratch) {
        mOrder.push_back(entry.mEntity);
    }
    ++mRebuilds;
}

void TransformHierarchy::Propagate(ecs::SystemContext& context) noexcept {
    if (CollectAndCheck(context)) {
        Rebuild(context);
    }
    for (const ecs::EntityId entity : mOrder) {
        const auto* const parent = context.Lookup<Parent>(mTypes.mParent, entity);
        if (parent == nullptr) {
            continue;
        }
        const auto* const parentMatrix = context.Lookup<LocalToWorld>(mTypes.mLocalToWorld, parent->mValue);
        const auto* const local        = context.Lookup<LocalToParent>(mTypes.mLocalToParent, entity);
        auto* const world              = context.LookupForWrite<LocalToWorld>(mTypes.mLocalToWorld, entity);
        if (parentMatrix == nullptr || local == nullptr || world == nullptr) {
            // A dead or matrix-less parent: the child keeps its last world matrix.
            continue;
        }
        Multiply(AffineConstRef{parentMatrix->mM}, AffineConstRef{local->mM}, AffineRef{world->mM});
    }
}

// --- registration -----------------------------------------------------------------------------

core::Expected<TransformSystemIds> AddTransformSystems(ecs::Schedule& schedule,
                                                       TransformHierarchy& hierarchy) noexcept {
    const TransformTypes& t = hierarchy.GetTypes();
    const ecs::ComponentMask trs =
        MaskOf(std::array<ecs::ComponentTypeIndex, 3>{t.mTranslation, t.mRotation, t.mScale});
    const ecs::ComponentMask optionalTrs = MaskOf(std::array<ecs::ComponentTypeIndex, 2>{t.mRotation, t.mScale});
    const ecs::ComponentMask manual      = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{t.mManualTransform});

    TransformSystemIds ids;
    const auto add = [&](const ecs::SystemDesc& desc, ecs::SystemId& slot) {
        const core::Expected<ecs::SystemId> result = schedule.AddSystem(desc);
        if (result.has_value()) {
            slot = *result;
        }
        return result.has_value();
    };
    if (!add(ecs::SystemDesc{.mName     = "transform.trs-to-world",
                             .mPhase    = ecs::Phase::TransformPropagate,
                             .mRead     = trs,
                             .mWrite    = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{t.mLocalToWorld}),
                             .mExclude  = manual.With(t.mParent),
                             .mOptional = optionalTrs,
                             .mSends    = ecs::MessageMask{},
                             .mReceives = ecs::MessageMask{},
                             .mFunction = &TrsToLocalToWorld,
                             .mData     = &hierarchy},
             ids.mTrsToWorld) ||
        !add(ecs::SystemDesc{.mName     = "transform.trs-to-parent",
                             .mPhase    = ecs::Phase::TransformPropagate,
                             .mRead     = trs.With(t.mParent),
                             .mWrite    = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{t.mLocalToParent}),
                             .mExclude  = manual,
                             .mOptional = optionalTrs,
                             .mSends    = ecs::MessageMask{},
                             .mReceives = ecs::MessageMask{},
                             .mFunction = &TrsToLocalToParent,
                             .mData     = &hierarchy},
             ids.mTrsToParent) ||
        !add(ecs::SystemDesc{.mName  = "transform.propagate",
                             .mPhase = ecs::Phase::TransformPropagate,
                             .mRead  = MaskOf(std::array<ecs::ComponentTypeIndex, 2>{t.mParent, t.mLocalToParent}),
                             .mWrite =
                                 MaskOf(std::array<ecs::ComponentTypeIndex, 2>{t.mLocalToWorld, t.mPreviousParent}),
                             .mExclude  = manual,
                             .mOptional = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{t.mPreviousParent}),
                             .mSends    = ecs::MessageMask{},
                             .mReceives = ecs::MessageMask{},
                             .mFunction = &PropagateSystem,
                             .mData     = &hierarchy},
             ids.mPropagate) ||
        !add(ecs::SystemDesc{.mName     = "transform.orphans",
                             .mPhase    = ecs::Phase::TransformPropagate,
                             .mRead     = ecs::ComponentMask{},
                             .mWrite    = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{t.mPreviousParent}),
                             .mExclude  = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{t.mParent}),
                             .mOptional = ecs::ComponentMask{},
                             .mSends    = ecs::MessageMask{},
                             .mReceives = ecs::MessageMask{},
                             .mFunction = &OrphanSystem,
                             .mData     = &hierarchy},
             ids.mOrphans)) {
        return core::Fail(core::ErrorCode::InvalidState, "transform: could not register the systems");
    }
    // Both composers feed the propagation; the orphan sweep touches PreviousParent after it.
    if (!schedule.AddOrder(ids.mTrsToWorld, ids.mPropagate).has_value() ||
        !schedule.AddOrder(ids.mTrsToParent, ids.mPropagate).has_value() ||
        !schedule.AddOrder(ids.mPropagate, ids.mOrphans).has_value()) {
        return core::Fail(core::ErrorCode::InvalidState, "transform: could not order the systems");
    }
    return ids;
}

} // namespace oyi::transform
