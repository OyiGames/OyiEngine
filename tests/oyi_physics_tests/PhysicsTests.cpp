#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_job/JobSystem.h>
#include <oyi_physics/Components.gen.h> // IWYU pragma: keep
#include <oyi_physics/Components.h>
#include <oyi_physics/PhysicsSystems.h>
#include <oyi_physics/PhysicsWorld.h>
#include <oyi_transform/Components.gen.h> // IWYU pragma: keep
#include <oyi_transform/Components.h>
#include <oyi_transform/TransformSystems.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <initializer_list>

namespace {

using Catch::Approx;
using oyi::core::SystemAllocator;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::Phase;
using oyi::ecs::Schedule;
using oyi::ecs::World;
using oyi::ecs::WorldKind;
using oyi::job::JobSystem;
using oyi::physics::AddPhysicsSystems;
using oyi::physics::BodyPose;
using oyi::physics::CollisionLayer;
using oyi::physics::kInvalidBodyId;
using oyi::physics::MotionType;
using oyi::physics::PhysicsBody;
using oyi::physics::PhysicsConfig;
using oyi::physics::PhysicsContext;
using oyi::physics::PhysicsStats;
using oyi::physics::PhysicsSystemIds;
using oyi::physics::PhysicsTypes;
using oyi::physics::PhysicsVelocity;
using oyi::physics::PhysicsWorld;
using oyi::physics::RegisterPhysicsComponents;
using oyi::physics::ShapeKind;
using oyi::transform::Float3;
using oyi::transform::RegisterTransformComponents;
using oyi::transform::Rotation;
using oyi::transform::TransformTypes;
using oyi::transform::Translation;

constexpr float kStep = 1.0f / 60.0f;

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

struct Fixture {
    SystemAllocator mAllocator;
    JobSystem mJobs{mAllocator,
                    JobSystem::Config{.mWorkerCount = 2, .mMaxJobs = 512, .mMaxCounters = 64, .mSpinIterations = 8}};
    World mWorld;
    TransformTypes mTransform{*RegisterTransformComponents(mWorld)};
    PhysicsTypes mTypes{*RegisterPhysicsComponents(mWorld)};
    PhysicsWorld mPhysics;
    PhysicsContext mContext{.mPhysics = &mPhysics, .mTransform = mTransform, .mTypes = mTypes};
    Schedule mSchedule{mAllocator, mWorld};
    PhysicsSystemIds mIds{};

    explicit Fixture(const PhysicsConfig& config = PhysicsConfig{})
        : mWorld(mAllocator, World::Config{.mChunkSize = 16 * 1024, .mKind = WorldKind::Server})
        , mPhysics(mAllocator, mJobs, config) {
        REQUIRE(mPhysics.IsValid());
        mWorld.GetEnvironmentForWrite().mFixedStepSeconds = kStep;
        mIds                                              = *AddPhysicsSystems(mSchedule, mContext);
        REQUIRE(mSchedule.Build().has_value());
    }

    [[nodiscard]] EntityId Spawn(const PhysicsBody& body, Float3 position, bool withVelocity = false) {
        ComponentMask mask = MaskOf({mTransform.mTranslation, mTransform.mRotation, mTypes.mPhysicsBody});
        if (withVelocity) {
            mask.Set(mTypes.mPhysicsVelocity);
        }
        const EntityId entity                           = *mWorld.CreateEntity(mask);
        mWorld.GetForWrite<Translation>(entity)->mValue = position;
        *mWorld.GetForWrite<PhysicsBody>(entity)        = body;
        return entity;
    }

    [[nodiscard]] EntityId Floor() {
        return Spawn(PhysicsBody{.mHalfExtent = Float3{.mX = 10.0f, .mY = 0.5f, .mZ = 10.0f},
                                 .mMotion     = MotionType::Static,
                                 .mShape      = ShapeKind::Box,
                                 .mLayer      = CollisionLayer::NonMoving},
                     Float3{.mX = 0.0f, .mY = -0.5f, .mZ = 0.0f}); // top face at y = 0
    }

    [[nodiscard]] EntityId Ball(float y, bool withVelocity = false) {
        return Spawn(PhysicsBody{.mHalfExtent = Float3{.mX = 0.5f, .mY = 0.5f, .mZ = 0.5f},
                                 .mMotion     = MotionType::Dynamic,
                                 .mShape      = ShapeKind::Sphere,
                                 .mLayer      = CollisionLayer::Moving},
                     Float3{.mX = 0.0f, .mY = y, .mZ = 0.0f}, withVelocity);
    }

    void Tick(bool parallel = true) {
        if (parallel) {
            REQUIRE(mSchedule.RunPhase(Phase::FixedSimulation, mJobs).has_value());
            REQUIRE(mSchedule.RunPhase(Phase::PhysicsSync, mJobs).has_value());
        } else {
            REQUIRE(mSchedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
            REQUIRE(mSchedule.RunPhaseSerial(Phase::PhysicsSync).has_value());
        }
    }

    [[nodiscard]] Float3 PositionOf(EntityId entity) const { return mWorld.Get<Translation>(entity)->mValue; }
};

} // namespace

TEST_CASE("a dynamic sphere falls onto a static box and comes to rest on it", "[physics][jolt]") {
    Fixture f;
    (void)f.Floor();
    const EntityId ball = f.Ball(4.0f, true);
    CHECK(f.mWorld.Get<PhysicsBody>(ball)->mBodyId == kInvalidBodyId);

    f.Tick();
    // The push system created both bodies on its first run; the ball has started to fall.
    CHECK(f.mWorld.Get<PhysicsBody>(ball)->mBodyId != kInvalidBodyId);
    CHECK(f.mPhysics.GetStats().mBodies == 2);
    for (int i = 0; i < 10; ++i) {
        f.Tick();
    }
    CHECK(f.PositionOf(ball).mY < 4.0f);
    CHECK(f.mWorld.Get<PhysicsVelocity>(ball)->mLinear.mY < -0.5f);

    for (int i = 0; i < 240; ++i) {
        f.Tick();
    }
    const Float3 rest = f.PositionOf(ball);
    CHECK(rest.mY == Approx(0.5f).margin(0.03f)); // radius above the floor's top face, less Jolt's penetration slop
    CHECK(rest.mX == Approx(0.0f).margin(0.01f));
    CHECK(rest.mZ == Approx(0.0f).margin(0.01f));
    CHECK(std::abs(f.mWorld.Get<PhysicsVelocity>(ball)->mLinear.mY) < 0.05f);
    const PhysicsStats stats = f.mPhysics.GetStats();
    CHECK(stats.mSteps == 251);
    CHECK(stats.mBodiesNotCreated == 0);
    CHECK(stats.mTempBytesHighWater > 0);
}

TEST_CASE("Jolt's jobs run through the engine JobSystem", "[physics][jolt][jobs]") {
    Fixture f;
    (void)f.Floor();
    for (int i = 0; i < 8; ++i) {
        (void)f.Ball(2.0f + (static_cast<float>(i) * 1.5f));
    }
    for (int i = 0; i < 30; ++i) {
        f.Tick();
    }
    const PhysicsStats stats = f.mPhysics.GetStats();
    CHECK(stats.mJoltJobs > 0);
    CHECK(stats.mJoltJobsOnWorkers + stats.mJoltJobsInline <= stats.mJoltJobs);
    CHECK(stats.mJoltJobsInline == 0); // the pools were big enough
    // The serial schedule path steps the same world: Jolt still fans out on the workers.
    const uint32_t before = stats.mJoltJobs;
    f.Tick(false);
    CHECK(f.mPhysics.GetStats().mJoltJobs > before);
}

TEST_CASE("a kinematic body follows the Translation gameplay writes and carries what rests on it", "[physics][jolt]") {
    Fixture f;
    const EntityId platform = f.Spawn(PhysicsBody{.mHalfExtent = Float3{.mX = 2.0f, .mY = 0.25f, .mZ = 2.0f},
                                                  .mMotion     = MotionType::Kinematic,
                                                  .mShape      = ShapeKind::Box,
                                                  .mLayer      = CollisionLayer::Moving},
                                      Float3{.mX = 0.0f, .mY = 0.0f, .mZ = 0.0f});
    const EntityId ball     = f.Ball(1.0f);
    for (int i = 0; i < 60; ++i) {
        f.Tick(); // settle on the platform
    }
    CHECK(f.PositionOf(ball).mY == Approx(0.75f).margin(0.03f));

    // Gameplay lifts the platform a centimetre per tick; the ball rides along.
    for (int i = 0; i < 120; ++i) {
        f.mWorld.GetForWrite<Translation>(platform)->mValue.mY += 0.01f;
        f.Tick();
    }
    BodyPose pose;
    REQUIRE(f.mPhysics.ReadPose(f.mWorld.Get<PhysicsBody>(platform)->mBodyId, pose));
    CHECK(pose.mPosition.mY == Approx(1.2f).margin(0.02f));
    CHECK(f.PositionOf(platform).mY == Approx(1.2f).margin(1e-5f)); // kinematic: gameplay owns it
    CHECK(f.PositionOf(ball).mY == Approx(1.95f).margin(0.1f));
}

TEST_CASE("bodies of destroyed entities leave the physics world at the next step", "[physics][jolt]") {
    Fixture f;
    (void)f.Floor();
    const EntityId a = f.Ball(2.0f);
    const EntityId b = f.Ball(5.0f);
    f.Tick();
    CHECK(f.mPhysics.GetStats().mBodies == 3);
    const uint32_t bodyA = f.mWorld.Get<PhysicsBody>(a)->mBodyId;
    REQUIRE(f.mWorld.DestroyEntity(a).has_value());
    f.Tick();
    CHECK(f.mPhysics.GetStats().mBodies == 2);
    BodyPose pose;
    CHECK_FALSE(f.mPhysics.ReadPose(bodyA, pose));
    CHECK(f.mPhysics.ReadPose(f.mWorld.Get<PhysicsBody>(b)->mBodyId, pose));
    CHECK(pose.mActive);
}

TEST_CASE("the body limit is reported, not crashed into", "[physics][jolt]") {
    Fixture f(PhysicsConfig{.mMaxBodies = 2});
    (void)f.Floor();
    (void)f.Ball(2.0f);
    const EntityId third = f.Ball(6.0f);
    f.Tick();
    const PhysicsStats stats = f.mPhysics.GetStats();
    CHECK(stats.mBodies == 2);
    CHECK(stats.mBodiesNotCreated >= 1);
    CHECK(f.mWorld.Get<PhysicsBody>(third)->mBodyId == kInvalidBodyId);
    // The row keeps asking every step and stays without a body while the limit holds.
    for (int i = 0; i < 3; ++i) {
        f.Tick();
    }
    CHECK(f.mWorld.Get<PhysicsBody>(third)->mBodyId == kInvalidBodyId);
    CHECK(f.mPhysics.GetStats().mBodiesNotCreated >= 4);
}

TEST_CASE("rotations round-trip through Jolt unchanged for static bodies", "[physics][jolt]") {
    Fixture f;
    const float half     = std::sqrt(0.5f);
    const EntityId crate = f.Spawn(PhysicsBody{.mMotion = MotionType::Dynamic, .mShape = ShapeKind::Box},
                                   Float3{.mX = 1.0f, .mY = 3.0f, .mZ = -2.0f});
    *f.mWorld.GetForWrite<Rotation>(crate) = Rotation{.mX = 0.0f, .mY = half, .mZ = 0.0f, .mW = half};
    f.Tick();
    BodyPose pose;
    REQUIRE(f.mPhysics.ReadPose(f.mWorld.Get<PhysicsBody>(crate)->mBodyId, pose));
    CHECK(pose.mRotation.mY == Approx(half).margin(1e-4f));
    CHECK(pose.mRotation.mW == Approx(half).margin(1e-4f));
    CHECK(pose.mPosition.mX == Approx(1.0f).margin(1e-4f));
    CHECK(pose.mPosition.mZ == Approx(-2.0f).margin(1e-4f));
    // A free-falling crate keeps its orientation.
    for (int i = 0; i < 30; ++i) {
        f.Tick();
    }
    CHECK(f.mWorld.Get<Rotation>(crate)->mY == Approx(half).margin(1e-3f));
    CHECK(f.PositionOf(crate).mY < 3.0f);
}
