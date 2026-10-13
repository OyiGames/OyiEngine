#include <oyi_core/Expected.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_transform/Components.h>
#include <oyi_transform/PresentationSystems.h>
#include <oyi_transform/TransformSystems.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oyi::transform {

namespace {

[[nodiscard]] ecs::ComponentMask MaskOf(std::span<const ecs::ComponentTypeIndex> types) noexcept {
    ecs::ComponentMask mask;
    for (const ecs::ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

/// RenderTransform::mPrevious = LocalToWorld, before the transform systems overwrite it.
void SampleSystem(ecs::SystemContext& context) {
    const auto* const state = context.GetData<PresentationContext>();
    context.ForEachChunk([&](ecs::ChunkView& view) {
        const std::span<const LocalToWorld> world = context.Read<LocalToWorld>(view, state->mTypes.mLocalToWorld);
        const std::span<RenderTransform> render = context.Write<RenderTransform>(view, state->mTypes.mRenderTransform);
        for (size_t i = 0; i < render.size(); ++i) {
            std::copy_n(world[i].mM, 12, render[i].mPrevious);
        }
    });
}

/// RenderTransform::mM = lerp(mPrevious, LocalToWorld, alpha). Element-wise: exact for the
/// translation, a fair approximation for small per-tick rotations.
void InterpolateSystem(ecs::SystemContext& context) {
    const auto* const state = context.GetData<PresentationContext>();
    const float alpha       = std::clamp(context.GetEnvironment().mInterpolationAlpha, 0.0f, 1.0f);
    context.ForEachChunk([&](ecs::ChunkView& view) {
        const std::span<const LocalToWorld> world = context.Read<LocalToWorld>(view, state->mTypes.mLocalToWorld);
        const std::span<RenderTransform> render = context.Write<RenderTransform>(view, state->mTypes.mRenderTransform);
        for (size_t i = 0; i < render.size(); ++i) {
            for (size_t k = 0; k < 12; ++k) {
                render[i].mM[k] = render[i].mPrevious[k] + ((world[i].mM[k] - render[i].mPrevious[k]) * alpha);
            }
        }
    });
}

/// One task per chunk: the chunk's WorldBounds from its translations. The signature is
/// ChunkTaskFunction's; this task only reads through its context.
void BoundsTask(ecs::SystemContext& task, ecs::ChunkView& view) { // NOLINT(misc-const-correctness)
    const auto* const state                   = task.GetData<PresentationContext>();
    const std::span<const LocalToWorld> world = task.Read<LocalToWorld>(view, state->mTypes.mLocalToWorld);
    auto* const bounds = static_cast<WorldBounds*>(view.GetChunkComponentForWrite(state->mTypes.mWorldBounds));
    if (bounds == nullptr) {
        return;
    }
    WorldBounds result;
    result.mCount = static_cast<uint32_t>(world.size());
    for (size_t i = 0; i < world.size(); ++i) {
        const Float3 position{.mX = world[i].mM[9], .mY = world[i].mM[10], .mZ = world[i].mM[11]};
        if (i == 0) {
            result.mMin = position;
            result.mMax = position;
            continue;
        }
        result.mMin = Float3{.mX = std::min(result.mMin.mX, position.mX),
                             .mY = std::min(result.mMin.mY, position.mY),
                             .mZ = std::min(result.mMin.mZ, position.mZ)};
        result.mMax = Float3{.mX = std::max(result.mMax.mX, position.mX),
                             .mY = std::max(result.mMax.mY, position.mY),
                             .mZ = std::max(result.mMax.mZ, position.mZ)};
    }
    *bounds = result;
}

void BoundsSystem(ecs::SystemContext& context) {
    const core::Result dispatched = context.ParallelForEachChunk(&BoundsTask);
    (void)dispatched; // a failed dispatch leaves the bounds of the previous frame in place
}

} // namespace

core::Expected<PresentationSystemIds> AddPresentationSystems(ecs::Schedule& schedule, PresentationContext& context,
                                                             const TransformSystemIds& transform) noexcept {
    const TransformTypes& t                  = context.mTypes;
    const ecs::ComponentMask localToWorld    = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{t.mLocalToWorld});
    const ecs::ComponentMask renderTransform = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{t.mRenderTransform});
    const ecs::ComponentMask worldBounds     = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{t.mWorldBounds});
    const ecs::ResourceMask environment      = ecs::ResourceMask{}.With(ecs::kEnvironmentResource);

    PresentationSystemIds ids;
    const auto add = [&](const ecs::SystemDesc& desc, ecs::WorldKindMask kinds, ecs::SystemId& slot) {
        const core::Expected<ecs::SystemId> result = schedule.AddSystem(desc, kinds);
        if (result.has_value()) {
            slot = *result;
        }
        return result.has_value();
    };
    if (!add(ecs::SystemDesc{.mName           = "transform.render-sample",
                             .mPhase          = ecs::Phase::TransformPropagate,
                             .mRead           = localToWorld,
                             .mWrite          = renderTransform,
                             .mExclude        = ecs::ComponentMask{},
                             .mOptional       = ecs::ComponentMask{},
                             .mSends          = ecs::MessageMask{},
                             .mReceives       = ecs::MessageMask{},
                             .mReadResources  = ecs::ResourceMask{},
                             .mWriteResources = ecs::ResourceMask{},
                             .mMainThread     = false,
                             .mFunction       = &SampleSystem,
                             .mData           = &context},
             ecs::kPresentationWorldKinds, ids.mSample) ||
        !add(ecs::SystemDesc{.mName           = "transform.render-interpolate",
                             .mPhase          = ecs::Phase::Presentation,
                             .mRead           = localToWorld,
                             .mWrite          = renderTransform,
                             .mExclude        = ecs::ComponentMask{},
                             .mOptional       = ecs::ComponentMask{},
                             .mSends          = ecs::MessageMask{},
                             .mReceives       = ecs::MessageMask{},
                             .mReadResources  = environment,
                             .mWriteResources = ecs::ResourceMask{},
                             .mMainThread     = false,
                             .mFunction       = &InterpolateSystem,
                             .mData           = &context},
             ecs::kPresentationWorldKinds, ids.mInterpolate) ||
        !add(ecs::SystemDesc{.mName           = "transform.world-bounds",
                             .mPhase          = ecs::Phase::TransformPropagate,
                             .mRead           = localToWorld,
                             .mWrite          = worldBounds,
                             .mExclude        = ecs::ComponentMask{},
                             .mOptional       = ecs::ComponentMask{},
                             .mSends          = ecs::MessageMask{},
                             .mReceives       = ecs::MessageMask{},
                             .mReadResources  = ecs::ResourceMask{},
                             .mWriteResources = ecs::ResourceMask{},
                             .mMainThread     = false,
                             .mFunction       = &BoundsSystem,
                             .mData           = &context},
             ecs::kAllWorldKinds, ids.mBounds)) {
        return core::Fail(core::ErrorCode::InvalidState, "transform: could not register the presentation systems");
    }
    // The sample reads LocalToWorld before the transform systems write it; the bounds after.
    const core::Result ordered = schedule.AddOrder(ids.mSample, transform.mTrsToWorld)
                                     .and_then([&] { return schedule.AddOrder(ids.mSample, transform.mPropagate); })
                                     .and_then([&] { return schedule.AddOrder(transform.mTrsToWorld, ids.mBounds); })
                                     .and_then([&] { return schedule.AddOrder(transform.mPropagate, ids.mBounds); });
    if (!ordered.has_value()) {
        return core::Fail(ordered.error().mCode, ordered.error().mMessage);
    }
    return ids;
}

} // namespace oyi::transform
