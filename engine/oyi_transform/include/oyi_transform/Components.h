#pragma once

#include <oyi_core/Annotations.h>
#include <oyi_ecs/Entity.h>

namespace oyi::transform {

/// Plain xyz; the simulation-side vector type until the math library lands (ADR-0018).
struct OYI_STRUCT() Float3 {
    float mX = 0.0f;
    float mY = 0.0f;
    float mZ = 0.0f;
};

/// Simulation-side TRS (ADR-0010): systems write these, the transform systems derive the
/// matrices. Metres, radians, right-handed, Y up (ADR-0014).
struct OYI_COMPONENT() Translation {
    Float3 mValue{};
};

/// Unit quaternion stored as (x, y, z, w) (ADR-0014).
struct OYI_COMPONENT() Rotation {
    float mX = 0.0f;
    float mY = 0.0f;
    float mZ = 0.0f;
    float mW = 1.0f;
};

struct OYI_COMPONENT() Scale {
    Float3 mValue{.mX = 1.0f, .mY = 1.0f, .mZ = 1.0f};
};

/// The one authoritative transform (ADR-0010): a 3x4 affine matrix, column-major, column vectors
/// (M * v); columns 0..2 are the basis, column 3 the translation. Static instances have only this.
struct OYI_COMPONENT() LocalToWorld {
    float mM[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
};

/// Transform relative to the parent; only entities with a Parent have it (ADR-0010).
struct OYI_COMPONENT() LocalToParent {
    float mM[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
};

/// Hierarchy link. Changing the parent is a structural intent in spirit: set it through the
/// command buffer at a sync point.
struct OYI_COMPONENT() Parent {
    oyi::ecs::EntityId mValue{};
};

/// System state (ADR-0010): what the propagation system saw as the parent last time. Added by the
/// propagation system itself, so entities need no initialisation when streamed in.
struct OYI_COMPONENT() PreviousParent {
    oyi::ecs::EntityId mValue{};
};

/// Opt-out tag: the built-in transform systems leave this entity's matrices alone.
struct OYI_COMPONENT(Tag) ManualTransform {};

} // namespace oyi::transform
