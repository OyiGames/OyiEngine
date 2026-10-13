#pragma once

#include <oyi_core/Expected.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_transform/TransformSystems.h>

namespace oyi::transform {

/// The presentation side of the transform (ADR-0010: simulation writes TRS, presentation
/// interpolates into the matrix the renderer reads; ADR-0005: chunk-level aggregation).
///
///   sample       TransformPropagate, before the transform systems: RenderTransform::mPrevious
///                takes the LocalToWorld of the previous fixed tick.
///   interpolate  Presentation, presentation worlds only: RenderTransform::mM blends mPrevious
///                and LocalToWorld by Environment::mInterpolationAlpha (the FixedStepper's).
///   bounds       TransformPropagate, after the transform systems, one task per chunk: the
///                chunk component WorldBounds holds the min / max of the chunk's translations.
struct PresentationSystemIds {
    ecs::SystemId mSample;
    ecs::SystemId mInterpolate;
    ecs::SystemId mBounds;
};

/// What the presentation systems need at run time; must outlive the schedule.
struct PresentationContext {
    TransformTypes mTypes;
};

[[nodiscard]] core::Expected<PresentationSystemIds>
AddPresentationSystems(ecs::Schedule& schedule, PresentationContext& context,
                       const TransformSystemIds& transform) noexcept;

} // namespace oyi::transform
