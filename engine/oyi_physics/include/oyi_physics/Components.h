#pragma once

#include <oyi_core/Annotations.h>
#include <oyi_transform/Components.h>

#include <cstdint>

namespace oyi::physics {

/// PhysicsBody::mBodyId before the push system has created the body.
inline constexpr uint32_t kInvalidBodyId = 0xFFFF'FFFFu;

enum class MotionType : uint8_t {
    Static    = 0, ///< never moves; its Translation / Rotation are read once at creation
    Kinematic = 1, ///< gameplay writes Translation / Rotation, the body follows (and pushes others)
    Dynamic   = 2, ///< the simulation writes Translation / Rotation
};

enum class ShapeKind : uint8_t {
    Box    = 0, ///< mHalfExtent is the half size on each axis
    Sphere = 1, ///< mHalfExtent.mX is the radius
};

/// Jolt object layers: static geometry does not collide with itself.
enum class CollisionLayer : uint8_t {
    NonMoving = 0,
    Moving    = 1,
};

/// A rigid body in the physics world (ADR-0018: Jolt). Add it next to Translation and Rotation
/// with the id invalid; the push system creates the body on its next run and fills the id in.
/// Shape sizes are world-space metres: Scale is not applied.
struct OYI_COMPONENT() PhysicsBody {
    uint32_t mBodyId              = kInvalidBodyId;
    transform::Float3 mHalfExtent = {.mX = 0.5f, .mY = 0.5f, .mZ = 0.5f};
    MotionType mMotion            = MotionType::Dynamic;
    ShapeKind mShape              = ShapeKind::Box;
    CollisionLayer mLayer         = CollisionLayer::Moving;
    uint8_t mPad                  = 0;
};

/// Optional read-back: the pull system writes the body's velocities here every fixed step.
struct OYI_COMPONENT() PhysicsVelocity {
    transform::Float3 mLinear{};
    transform::Float3 mAngular{}; ///< radians per second
};

} // namespace oyi::physics
