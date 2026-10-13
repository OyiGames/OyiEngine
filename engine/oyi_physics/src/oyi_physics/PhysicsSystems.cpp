#include <oyi_core/Expected.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_physics/Components.gen.h> // IWYU pragma: keep
#include <oyi_physics/Components.h>
#include <oyi_physics/PhysicsSystems.h>
#include <oyi_physics/PhysicsWorld.h>
#include <oyi_transform/Components.h>
#include <oyi_transform/TransformSystems.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oyi::physics {

namespace {

[[nodiscard]] ecs::ComponentMask MaskOf(std::span<const ecs::ComponentTypeIndex> types) noexcept {
    ecs::ComponentMask mask;
    for (const ecs::ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

/// Bodies for new rows; kinematic bodies move to what gameplay wrote.
void PushSystem(ecs::SystemContext& context) {
    const auto* const state = context.GetData<PhysicsContext>();
    const float step        = context.GetEnvironment().mFixedStepSeconds;
    context.ForEachChunk([&](ecs::ChunkView& view) {
        const std::span<const ecs::EntityId> entities = view.GetEntities();
        const std::span<PhysicsBody> bodies           = context.Write<PhysicsBody>(view, state->mTypes.mPhysicsBody);
        const std::span<const transform::Translation> translations =
            context.Read<transform::Translation>(view, state->mTransform.mTranslation);
        const std::span<const transform::Rotation> rotations =
            context.Read<transform::Rotation>(view, state->mTransform.mRotation);
        for (size_t i = 0; i < bodies.size(); ++i) {
            PhysicsBody& body = bodies[i];
            if (body.mBodyId == kInvalidBodyId) {
                const core::Expected<uint32_t> created =
                    state->mPhysics->CreateBody(body, translations[i].mValue, rotations[i], entities[i]);
                if (created.has_value()) {
                    body.mBodyId = *created;
                }
                continue; // the pose it was created with is the pose
            }
            if (body.mMotion == MotionType::Kinematic) {
                state->mPhysics->MoveKinematic(body.mBodyId, translations[i].mValue, rotations[i], step);
            }
        }
    });
}

/// Main thread: dead owners first, then the step itself. The signature is SystemFunction's; the
/// step only reads through its context.
void StepSystem(ecs::SystemContext& context) { // NOLINT(misc-const-correctness)
    const auto* const state = context.GetData<PhysicsContext>();
    (void)state->mPhysics->CollectDead(context.GetWorld());
    const core::Result stepped = state->mPhysics->Step(context.GetEnvironment().mFixedStepSeconds);
    (void)stepped; // an overflowed Jolt buffer is reported through the stats; the step still ran
}

/// Dynamic bodies write their pose (and velocity, where the row has PhysicsVelocity) back.
void PullSystem(ecs::SystemContext& context) {
    const auto* const state = context.GetData<PhysicsContext>();
    context.ForEachChunk([&](ecs::ChunkView& view) {
        const std::span<const PhysicsBody> bodies = context.Read<PhysicsBody>(view, state->mTypes.mPhysicsBody);
        const std::span<transform::Translation> translations =
            context.Write<transform::Translation>(view, state->mTransform.mTranslation);
        const std::span<transform::Rotation> rotations =
            context.Write<transform::Rotation>(view, state->mTransform.mRotation);
        const bool hasVelocity = view.Has(state->mTypes.mPhysicsVelocity);
        const std::span<PhysicsVelocity> velocities =
            hasVelocity ? context.Write<PhysicsVelocity>(view, state->mTypes.mPhysicsVelocity)
                        : std::span<PhysicsVelocity>{};
        BodyPose pose;
        for (size_t i = 0; i < bodies.size(); ++i) {
            if (bodies[i].mMotion != MotionType::Dynamic || !state->mPhysics->ReadPose(bodies[i].mBodyId, pose)) {
                continue;
            }
            translations[i].mValue = pose.mPosition;
            rotations[i]           = pose.mRotation;
            if (hasVelocity) {
                velocities[i].mLinear  = pose.mLinearVelocity;
                velocities[i].mAngular = pose.mAngularVelocity;
            }
        }
    });
}

} // namespace

core::Expected<PhysicsTypes> RegisterPhysicsComponents(ecs::World& world) noexcept {
    ecs::ComponentRegistry& registry = world.GetComponents();
    PhysicsTypes types;
    const core::Expected<ecs::ComponentTypeIndex> body     = registry.Register<PhysicsBody>();
    const core::Expected<ecs::ComponentTypeIndex> velocity = registry.Register<PhysicsVelocity>();
    if (!body.has_value() || !velocity.has_value()) {
        return core::Fail(core::ErrorCode::OutOfRange, "physics: component registry full");
    }
    types.mPhysicsBody     = *body;
    types.mPhysicsVelocity = *velocity;
    return types;
}

core::Expected<PhysicsSystemIds> AddPhysicsSystems(ecs::Schedule& schedule, PhysicsContext& context) noexcept {
    if (context.mPhysics == nullptr || !context.mPhysics->IsValid()) {
        return core::Fail(core::ErrorCode::InvalidArgument, "physics: the context needs a valid PhysicsWorld");
    }
    const transform::TransformTypes& t  = context.mTransform;
    const PhysicsTypes& p               = context.mTypes;
    const ecs::ComponentMask pose       = MaskOf(std::array<ecs::ComponentTypeIndex, 2>{t.mTranslation, t.mRotation});
    const ecs::ComponentMask body       = MaskOf(std::array<ecs::ComponentTypeIndex, 1>{p.mPhysicsBody});
    const ecs::ComponentMask velocity   = ecs::ComponentMask{}.With(p.mPhysicsVelocity);
    const ecs::ResourceMask environment = ecs::ResourceMask{}.With(ecs::kEnvironmentResource);

    PhysicsSystemIds ids;
    const auto add = [&](const ecs::SystemDesc& desc, ecs::SystemId& slot) {
        const core::Expected<ecs::SystemId> result = schedule.AddSystem(desc, ecs::kAllWorldKinds);
        if (result.has_value()) {
            slot = *result;
        }
        return result.has_value();
    };
    if (!add(ecs::SystemDesc{.mName           = "physics.push",
                             .mPhase          = ecs::Phase::FixedSimulation,
                             .mRead           = pose,
                             .mWrite          = body,
                             .mExclude        = ecs::ComponentMask{},
                             .mOptional       = ecs::ComponentMask{},
                             .mSends          = ecs::MessageMask{},
                             .mReceives       = ecs::MessageMask{},
                             .mReadResources  = environment,
                             .mWriteResources = ecs::ResourceMask{},
                             .mMainThread     = false,
                             .mFunction       = &PushSystem,
                             .mData           = &context},
             ids.mPush) ||
        !add(ecs::SystemDesc{.mName           = "physics.step",
                             .mPhase          = ecs::Phase::FixedSimulation,
                             .mRead           = ecs::ComponentMask{},
                             .mWrite          = ecs::ComponentMask{},
                             .mExclude        = ecs::ComponentMask{},
                             .mOptional       = ecs::ComponentMask{},
                             .mSends          = ecs::MessageMask{},
                             .mReceives       = ecs::MessageMask{},
                             .mReadResources  = environment,
                             .mWriteResources = ecs::ResourceMask{},
                             .mMainThread     = true,
                             .mFunction       = &StepSystem,
                             .mData           = &context},
             ids.mStep) ||
        !add(ecs::SystemDesc{.mName           = "physics.pull",
                             .mPhase          = ecs::Phase::PhysicsSync,
                             .mRead           = body,
                             .mWrite          = pose.With(p.mPhysicsVelocity),
                             .mExclude        = ecs::ComponentMask{},
                             .mOptional       = velocity,
                             .mSends          = ecs::MessageMask{},
                             .mReceives       = ecs::MessageMask{},
                             .mReadResources  = ecs::ResourceMask{},
                             .mWriteResources = ecs::ResourceMask{},
                             .mMainThread     = false,
                             .mFunction       = &PullSystem,
                             .mData           = &context},
             ids.mPull)) {
        return core::Fail(core::ErrorCode::InvalidState, "physics: could not register the physics systems");
    }
    const core::Result ordered = schedule.AddOrder(ids.mPush, ids.mStep);
    if (!ordered.has_value()) {
        return core::Fail(ordered.error().mCode, ordered.error().mMessage);
    }
    return ids;
}

} // namespace oyi::physics
