#pragma once

#include <oyi_core/Expected.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_physics/PhysicsWorld.h>
#include <oyi_transform/TransformSystems.h>

namespace oyi::physics {

struct PhysicsTypes {
    ecs::ComponentTypeIndex mPhysicsBody;
    ecs::ComponentTypeIndex mPhysicsVelocity;
};

[[nodiscard]] core::Expected<PhysicsTypes> RegisterPhysicsComponents(ecs::World& world) noexcept;

/// What the physics systems need at run time; must outlive the schedule.
struct PhysicsContext {
    PhysicsWorld* mPhysics = nullptr;
    transform::TransformTypes mTransform;
    PhysicsTypes mTypes;
};

/// The physics side of the fixed step (ADR-0007 phases, ADR-0018 Jolt):
///
///   push   FixedSimulation: creates bodies for PhysicsBody rows without one, moves kinematic
///          bodies to the Translation / Rotation gameplay wrote.
///   step   FixedSimulation, after push, main thread: collects the bodies of dead entities, then
///          PhysicsWorld::Step(Environment::mFixedStepSeconds).
///   pull   PhysicsSync: Translation / Rotation (and PhysicsVelocity when present) of dynamic
///          bodies take the simulated pose.
struct PhysicsSystemIds {
    ecs::SystemId mPush;
    ecs::SystemId mStep;
    ecs::SystemId mPull;
};

[[nodiscard]] core::Expected<PhysicsSystemIds> AddPhysicsSystems(ecs::Schedule& schedule,
                                                                 PhysicsContext& context) noexcept;

} // namespace oyi::physics
