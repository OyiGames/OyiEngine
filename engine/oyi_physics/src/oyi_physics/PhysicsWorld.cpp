#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>
#include <oyi_job/JobSystem.h>
#include <oyi_physics/Components.h>
#include <oyi_physics/PhysicsWorld.h>
#include <oyi_physics/detail/Jolt.h>
#include <oyi_physics/detail/JoltJobSystem.h>
#include <oyi_transform/Components.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace oyi::physics {

namespace {

constexpr JPH::ObjectLayer kLayerNonMoving = 0;
constexpr JPH::ObjectLayer kLayerMoving    = 1;
constexpr JPH::uint kObjectLayerCount      = 2;
constexpr JPH::uint kBroadPhaseLayerCount  = 2;
constexpr JPH::uint kBodyMutexCount        = 0; // Jolt picks

// --- Jolt's process-wide state, shared by every PhysicsWorld ---------------------------------------

// Jolt's TraceFunction is C-variadic; this is the one place that follows it.
void TraceToStderr(const char* format, ...) { // NOLINT(modernize-avoid-variadic-functions)
    std::array<char, 512> line{};
    va_list arguments{};
    va_start(arguments, format);
    (void)std::vsnprintf(line.data(), line.size(), format, arguments);
    va_end(arguments);
    (void)std::fputs("jolt: ", stderr);
    (void)std::fputs(line.data(), stderr);
    (void)std::fputc('\n', stderr);
}

#ifdef JPH_ENABLE_ASSERTS
bool OnAssertFailed(const char* expression, const char* message, const char* file, JPH::uint line) {
    (void)std::fprintf(stderr, "jolt assert: %s:%u: %s %s\n", file, line, expression,
                       message != nullptr ? message : "");
    return true; // break into the debugger
}
#endif

class Runtime {
public:
    static void Acquire() noexcept {
        const std::scoped_lock lock(sMutex);
        if (sUsers++ == 0) {
            JPH::RegisterDefaultAllocator();
            JPH::Trace = &TraceToStderr;
            JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = &OnAssertFailed;)
            // Factory's operator new is Jolt's allocator, which aborts rather than throws.
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,bugprone-unhandled-exception-at-new)
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }
    }

    static void Release() noexcept {
        const std::scoped_lock lock(sMutex);
        if (--sUsers == 0) {
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance; // NOLINT(cppcoreguidelines-owning-memory)
            JPH::Factory::sInstance = nullptr;
        }
    }

private:
    static std::mutex sMutex;
    static int sUsers;
};

std::mutex Runtime::sMutex;
int Runtime::sUsers = 0;

// --- Jolt's scratch memory from the engine allocator ----------------------------------------------

/// Jolt's per-step scratch is strictly LIFO, so one block and a top pointer are enough.
class StackTempAllocator final : public JPH::TempAllocator {
public:
    StackTempAllocator(core::Allocator& allocator, size_t bytes) noexcept
        : mAllocator(allocator)
        , mSize(bytes)
        , mBase(static_cast<std::byte*>(allocator.Allocate(bytes, JPH_RVECTOR_ALIGNMENT))) {}
    StackTempAllocator(const StackTempAllocator&)            = delete;
    StackTempAllocator(StackTempAllocator&&)                 = delete;
    StackTempAllocator& operator=(const StackTempAllocator&) = delete;
    StackTempAllocator& operator=(StackTempAllocator&&)      = delete;
    ~StackTempAllocator() override {
        OYI_CHECK(mTop == 0, "physics: Jolt scratch memory still in use");
        mAllocator.Free(mBase, mSize, JPH_RVECTOR_ALIGNMENT);
    }

    [[nodiscard]] bool IsValid() const noexcept { return mBase != nullptr; }
    [[nodiscard]] size_t GetHighWater() const noexcept { return mHighWater; }

    void* Allocate(JPH::uint size) override {
        if (size == 0) {
            return nullptr;
        }
        const size_t rounded = JPH::AlignUp(static_cast<size_t>(size), JPH_RVECTOR_ALIGNMENT);
        OYI_CHECK(mTop + rounded <= mSize, "physics: Jolt scratch exhausted; raise PhysicsConfig::mTempBytes");
        const size_t offset = mTop;
        mTop += rounded;
        mHighWater = std::max(mHighWater, mTop);
        return mBase + offset;
    }

    void Free(void* address, JPH::uint size) override {
        if (address == nullptr) {
            return;
        }
        mTop -= JPH::AlignUp(static_cast<size_t>(size), JPH_RVECTOR_ALIGNMENT);
        OYI_CHECK(mBase + mTop == address, "physics: Jolt scratch freed out of order");
    }

private:
    core::Allocator& mAllocator;
    size_t mSize;
    std::byte* mBase;
    size_t mTop       = 0;
    size_t mHighWater = 0;
};

// --- conversions (ADR-0014: both sides are metres, right-handed, Y up, (x, y, z, w)) --------------

[[nodiscard]] JPH::Vec3 ToJolt(const transform::Float3& value) noexcept {
    return {value.mX, value.mY, value.mZ};
}

[[nodiscard]] transform::Float3 FromJolt(JPH::Vec3Arg value) noexcept {
    return transform::Float3{.mX = value.GetX(), .mY = value.GetY(), .mZ = value.GetZ()};
}

[[nodiscard]] JPH::Quat ToJolt(const transform::Rotation& value) noexcept {
    return {value.mX, value.mY, value.mZ, value.mW};
}

[[nodiscard]] transform::Rotation FromJolt(JPH::QuatArg value) noexcept {
    return transform::Rotation{.mX = value.GetX(), .mY = value.GetY(), .mZ = value.GetZ(), .mW = value.GetW()};
}

[[nodiscard]] JPH::EMotionType ToJolt(MotionType motion) noexcept {
    switch (motion) {
    case MotionType::Static:
        return JPH::EMotionType::Static;
    case MotionType::Kinematic:
        return JPH::EMotionType::Kinematic;
    case MotionType::Dynamic:
        return JPH::EMotionType::Dynamic;
    }
    return JPH::EMotionType::Static;
}

[[nodiscard]] JPH::ObjectLayer ToJolt(CollisionLayer layer) noexcept {
    return layer == CollisionLayer::NonMoving ? kLayerNonMoving : kLayerMoving;
}

[[nodiscard]] JPH::ShapeSettings::ShapeResult MakeShape(const PhysicsBody& body) noexcept {
    if (body.mShape == ShapeKind::Sphere) {
        const JPH::SphereShapeSettings settings(body.mHalfExtent.mX);
        return settings.Create();
    }
    // Jolt wants the convex radius below every half extent; small boxes get a smaller one.
    const float smallest     = std::min({body.mHalfExtent.mX, body.mHalfExtent.mY, body.mHalfExtent.mZ});
    const float convexRadius = std::min(JPH::cDefaultConvexRadius, smallest * 0.5f);
    const JPH::BoxShapeSettings settings(ToJolt(body.mHalfExtent), convexRadius);
    return settings.Create();
}

// --- layers: the object-vs-broadphase table reads the two below when it is built, so they are
// complete before Impl's member initialisers reach it --------------------------------------------

struct BroadPhaseLayers final : JPH::BroadPhaseLayerInterfaceTable {
    BroadPhaseLayers() : JPH::BroadPhaseLayerInterfaceTable(kObjectLayerCount, kBroadPhaseLayerCount) {
        MapObjectToBroadPhaseLayer(kLayerNonMoving, JPH::BroadPhaseLayer(0));
        MapObjectToBroadPhaseLayer(kLayerMoving, JPH::BroadPhaseLayer(1));
    }
};

struct LayerPairs final : JPH::ObjectLayerPairFilterTable {
    LayerPairs() : JPH::ObjectLayerPairFilterTable(kObjectLayerCount) {
        EnableCollision(kLayerMoving, kLayerMoving);
        EnableCollision(kLayerMoving, kLayerNonMoving); // static geometry never meets itself
    }
};

} // namespace

struct PhysicsWorld::Impl {
    Impl(core::Allocator& allocator, job::JobSystem& jobs, const PhysicsConfig& config) noexcept
        : mJobs(jobs, config.mMaxJoltJobs, config.mMaxJoltBarriers)
        , mTemp(allocator, config.mTempBytes)
        , mObjectVsBroadPhase(mBroadPhase, kBroadPhaseLayerCount, mPairs, kObjectLayerCount) {
        mSystem.Init(config.mMaxBodies, kBodyMutexCount, config.mMaxBodyPairs, config.mMaxContactConstraints,
                     mBroadPhase, mObjectVsBroadPhase, mPairs);
        mSystem.SetGravity(ToJolt(config.mGravity));
    }

    detail::JoltJobSystem mJobs;
    StackTempAllocator mTemp;
    BroadPhaseLayers mBroadPhase;
    LayerPairs mPairs;
    JPH::ObjectVsBroadPhaseLayerFilterTable mObjectVsBroadPhase;
    JPH::PhysicsSystem mSystem;
    uint32_t mSteps            = 0;
    uint32_t mBodiesNotCreated = 0;
};

PhysicsWorld::PhysicsWorld(core::Allocator& allocator, job::JobSystem& jobs, const PhysicsConfig& config) noexcept
    : mAllocator(allocator) {
    Runtime::Acquire();
    mImpl = allocator.New<Impl>(allocator, jobs, config);
    if (mImpl != nullptr && !mImpl->mTemp.IsValid()) {
        allocator.Delete(mImpl);
        mImpl = nullptr;
    }
    if (mImpl == nullptr) {
        Runtime::Release();
    }
}

PhysicsWorld::~PhysicsWorld() {
    if (mImpl != nullptr) {
        mAllocator.Delete(mImpl);
        Runtime::Release();
    }
}

core::Expected<uint32_t> PhysicsWorld::CreateBody(const PhysicsBody& body, const transform::Float3& position,
                                                  const transform::Rotation& rotation, ecs::EntityId owner) noexcept {
    if (mImpl == nullptr) {
        return core::Fail(core::ErrorCode::InvalidState, "physics: world not initialised");
    }
    if (mImpl->mSystem.GetNumBodies() >= mImpl->mSystem.GetMaxBodies()) {
        ++mImpl->mBodiesNotCreated;
        return core::Fail(core::ErrorCode::OutOfRange, "physics: body limit reached (PhysicsConfig::mMaxBodies)");
    }
    const JPH::ShapeSettings::ShapeResult shape = MakeShape(body);
    if (!shape.IsValid()) {
        ++mImpl->mBodiesNotCreated;
        return core::Fail(core::ErrorCode::InvalidArgument, "physics: the shape could not be built");
    }
    JPH::BodyCreationSettings settings(shape.Get(), JPH::RVec3(ToJolt(position)), ToJolt(rotation),
                                       ToJolt(body.mMotion), ToJolt(body.mLayer));
    settings.mUserData   = owner.ToU64();
    const JPH::BodyID id = mImpl->mSystem.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::Activate);
    if (id.IsInvalid()) {
        ++mImpl->mBodiesNotCreated;
        return core::Fail(core::ErrorCode::OutOfRange, "physics: Jolt refused the body");
    }
    return id.GetIndexAndSequenceNumber();
}

void PhysicsWorld::DestroyBody(uint32_t bodyId) noexcept {
    if (mImpl == nullptr || bodyId == kInvalidBodyId) {
        return;
    }
    const JPH::BodyID id(bodyId);
    JPH::BodyInterface& bodies = mImpl->mSystem.GetBodyInterface();
    if (!bodies.IsAdded(id)) {
        return;
    }
    bodies.RemoveBody(id);
    bodies.DestroyBody(id);
}

void PhysicsWorld::MoveKinematic(uint32_t bodyId, const transform::Float3& position,
                                 const transform::Rotation& rotation, float deltaSeconds) noexcept {
    if (mImpl == nullptr || bodyId == kInvalidBodyId) {
        return;
    }
    const JPH::BodyID id(bodyId);
    JPH::BodyInterface& bodies = mImpl->mSystem.GetBodyInterface();
    if (!bodies.IsAdded(id)) {
        return;
    }
    if (deltaSeconds > 0.0f) {
        bodies.MoveKinematic(id, JPH::RVec3(ToJolt(position)), ToJolt(rotation), deltaSeconds);
    } else {
        bodies.SetPositionAndRotation(id, JPH::RVec3(ToJolt(position)), ToJolt(rotation), JPH::EActivation::Activate);
    }
}

bool PhysicsWorld::ReadPose(uint32_t bodyId, BodyPose& out) const noexcept {
    if (mImpl == nullptr || bodyId == kInvalidBodyId) {
        return false;
    }
    const JPH::BodyID id(bodyId);
    const JPH::BodyInterface& bodies = mImpl->mSystem.GetBodyInterface();
    if (!bodies.IsAdded(id)) {
        return false;
    }
    JPH::RVec3 position;
    JPH::Quat rotation;
    bodies.GetPositionAndRotation(id, position, rotation);
    out.mPosition        = FromJolt(JPH::Vec3(position));
    out.mRotation        = FromJolt(rotation);
    out.mLinearVelocity  = FromJolt(bodies.GetLinearVelocity(id));
    out.mAngularVelocity = FromJolt(bodies.GetAngularVelocity(id));
    out.mActive          = bodies.IsActive(id);
    return true;
}

core::Result PhysicsWorld::Step(float deltaSeconds, int collisionSteps) noexcept {
    if (mImpl == nullptr) {
        return core::Fail(core::ErrorCode::InvalidState, "physics: world not initialised");
    }
    const JPH::EPhysicsUpdateError error =
        mImpl->mSystem.Update(deltaSeconds, std::max(collisionSteps, 1), &mImpl->mTemp, &mImpl->mJobs);
    ++mImpl->mSteps;
    if (error != JPH::EPhysicsUpdateError::None) {
        return core::Fail(core::ErrorCode::OutOfRange,
                          "physics: a Jolt buffer overflowed (body pairs or contact constraints)");
    }
    return {};
}

uint32_t PhysicsWorld::CollectDead(const ecs::World& world) noexcept {
    if (mImpl == nullptr) {
        return 0;
    }
    JPH::BodyIDVector ids;
    mImpl->mSystem.GetBodies(ids);
    JPH::BodyInterface& bodies = mImpl->mSystem.GetBodyInterface();
    uint32_t collected         = 0;
    for (const JPH::BodyID id : ids) {
        const ecs::EntityId owner = ecs::EntityId::FromU64(bodies.GetUserData(id));
        if (world.IsAlive(owner)) {
            continue;
        }
        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
        ++collected;
    }
    return collected;
}

PhysicsStats PhysicsWorld::GetStats() const noexcept {
    PhysicsStats stats;
    if (mImpl == nullptr) {
        return stats;
    }
    const detail::JoltJobSystem::Stats jobs = mImpl->mJobs.GetStats();
    stats.mBodies                           = mImpl->mSystem.GetNumBodies();
    stats.mActiveBodies                     = mImpl->mSystem.GetNumActiveBodies(JPH::EBodyType::RigidBody);
    stats.mSteps                            = mImpl->mSteps;
    stats.mJoltJobs                         = jobs.mJobs;
    stats.mJoltJobsOnWorkers                = jobs.mOnWorkers;
    stats.mJoltJobsInline                   = jobs.mInline;
    stats.mBodiesNotCreated                 = mImpl->mBodiesNotCreated;
    stats.mTempBytesHighWater               = static_cast<uint32_t>(mImpl->mTemp.GetHighWater());
    return stats;
}

} // namespace oyi::physics
