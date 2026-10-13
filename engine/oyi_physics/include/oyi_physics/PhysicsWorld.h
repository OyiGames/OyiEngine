#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>
#include <oyi_job/JobSystem.h>
#include <oyi_physics/Components.h>
#include <oyi_transform/Components.h>

#include <cstdint>

namespace oyi::physics {

struct PhysicsConfig {
    uint32_t mMaxBodies             = 1024;
    uint32_t mMaxBodyPairs          = 1024;
    uint32_t mMaxContactConstraints = 1024;
    uint32_t mTempBytes             = 4u * 1024u * 1024u; ///< Jolt's per-step scratch, LIFO
    uint32_t mMaxJoltJobs           = 1024;               ///< Jolt jobs in flight at once
    uint32_t mMaxJoltBarriers       = 4;
    transform::Float3 mGravity      = {.mX = 0.0f, .mY = -9.81f, .mZ = 0.0f};
};

struct PhysicsStats {
    uint32_t mBodies             = 0;
    uint32_t mActiveBodies       = 0;
    uint32_t mSteps              = 0;
    uint32_t mJoltJobs           = 0; ///< jobs Jolt asked for, all run through the engine JobSystem
    uint32_t mJoltJobsOnWorkers  = 0; ///< of those, executed on a worker thread
    uint32_t mJoltJobsInline     = 0; ///< run on the spot because the job pool was full
    uint32_t mBodiesNotCreated   = 0; ///< CreateBody failures (the body limit, a bad shape)
    uint32_t mTempBytesHighWater = 0;
};

struct BodyPose {
    transform::Float3 mPosition{};
    transform::Rotation mRotation{};
    transform::Float3 mLinearVelocity{};
    transform::Float3 mAngularVelocity{};
    bool mActive = false; ///< false once Jolt put the body to sleep
};

/// One Jolt PhysicsSystem (ADR-0018) stepped on the engine's JobSystem (ADR-0009: Jolt's jobs go
/// through our scheduler, not a thread pool of its own). Conventions match ADR-0014 on both
/// sides: metres, right-handed, Y up, quaternions (x, y, z, w). The public surface has no Jolt
/// in it; the systems in PhysicsSystems.h drive it from the ECS.
class PhysicsWorld {
public:
    PhysicsWorld(core::Allocator& allocator, job::JobSystem& jobs, const PhysicsConfig& config) noexcept;
    PhysicsWorld(const PhysicsWorld&)            = delete;
    PhysicsWorld(PhysicsWorld&&)                 = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(PhysicsWorld&&)      = delete;
    ~PhysicsWorld();

    /// False when the constructor could not allocate; every other call is then a no-op.
    [[nodiscard]] bool IsValid() const noexcept { return mImpl != nullptr; }

    /// Creates a body from the component and its pose; `owner` is remembered so that CollectDead
    /// can find bodies whose entity died. Fails with OutOfRange at the body limit.
    [[nodiscard]] core::Expected<uint32_t> CreateBody(const PhysicsBody& body, const transform::Float3& position,
                                                      const transform::Rotation& rotation,
                                                      ecs::EntityId owner) noexcept;
    void DestroyBody(uint32_t bodyId) noexcept;
    /// Moves a kinematic body so that it arrives at the pose after `deltaSeconds`.
    void MoveKinematic(uint32_t bodyId, const transform::Float3& position, const transform::Rotation& rotation,
                       float deltaSeconds) noexcept;
    /// False when the id is not a body in this world.
    [[nodiscard]] bool ReadPose(uint32_t bodyId, BodyPose& out) const noexcept;

    /// One fixed step. Call from the thread that owns the JobSystem (the main thread): Jolt's jobs
    /// run on the workers and this thread helps while it waits.
    [[nodiscard]] core::Result Step(float deltaSeconds, int collisionSteps = 1) noexcept;
    /// Destroys bodies whose owning entity is no longer alive; returns how many.
    uint32_t CollectDead(const ecs::World& world) noexcept;

    [[nodiscard]] PhysicsStats GetStats() const noexcept;

private:
    struct Impl;
    core::Allocator& mAllocator;
    Impl* mImpl = nullptr;
};

} // namespace oyi::physics
