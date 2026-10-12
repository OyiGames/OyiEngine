#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_job/JobSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::Environment;
using oyi::ecs::FirewallWindow;
using oyi::ecs::kEnvironmentResource;
using oyi::ecs::kEnvironmentResourceName;
using oyi::ecs::kMaxResourceTypes;
using oyi::ecs::ManagedContext;
using oyi::ecs::ManagedId;
using oyi::ecs::ManagedObject;
using oyi::ecs::ManagedPool;
using oyi::ecs::ManagedSystem;
using oyi::ecs::MessageMask;
using oyi::ecs::Phase;
using oyi::ecs::ResourceMask;
using oyi::ecs::ResourceOrdering;
using oyi::ecs::ResourceRegistry;
using oyi::ecs::ResourceTypeIndex;
using oyi::ecs::Schedule;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::SystemId;
using oyi::ecs::World;
using oyi::job::JobSystem;
using namespace oyi::ecs_fixture;

constexpr JobSystem::Config kThreeWorkers{
    .mWorkerCount = 3, .mMaxJobs = 256, .mMaxCounters = 128, .mSpinIterations = 8};

/// Stand-ins for the physics world and the random source a game would register.
struct PhysicsWorld {
    uint32_t mSteps = 0;
    float mGravity  = -9.8f;
};

struct Rng {
    uint64_t mState = 1;
};

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

[[nodiscard]] ResourceMask ResourcesOf(std::initializer_list<ResourceTypeIndex> types) {
    ResourceMask mask;
    for (const ResourceTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

[[nodiscard]] SystemDesc Describe(std::string_view name, Phase phase, ComponentMask read, ComponentMask write,
                                  ResourceMask readResources, ResourceMask writeResources,
                                  oyi::ecs::SystemFunction function, void* data) {
    return SystemDesc{.mName           = name,
                      .mPhase          = phase,
                      .mRead           = read,
                      .mWrite          = write,
                      .mExclude        = ComponentMask{},
                      .mOptional       = ComponentMask{},
                      .mSends          = MessageMask{},
                      .mReceives       = MessageMask{},
                      .mReadResources  = readResources,
                      .mWriteResources = writeResources,
                      .mMainThread     = false,
                      .mFunction       = function,
                      .mData           = data};
}

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mBrainRef = *mWorld.GetComponents().Register<BrainRef>();
    PhysicsWorld mPhysics;
    Rng mRng;
    ResourceTypeIndex mPhysicsResource = *mWorld.GetResources().Register("test::PhysicsWorld", mPhysics);
    ResourceTypeIndex mRngResource     = *mWorld.GetResources().Register("test::Rng", mRng);
    Schedule mSchedule{mAllocator, mWorld};
};

void Nothing(SystemContext& /*context*/) {}

/// Only safe because every system that touches it is ordered against the others.
struct Trace {
    std::vector<std::string_view> mOrder;
    ResourceTypeIndex mPhysics;
    ResourceTypeIndex mRng;
    bool mCanReadPhysics  = false;
    bool mCanWritePhysics = false;
    bool mCanReadRng      = false;
};

void WriteEnvironmentA(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("A");
    ++context.GetEnvironmentForWrite().mFrameIndex;
}
void WriteEnvironmentB(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("B");
    ++context.GetEnvironmentForWrite().mFrameIndex;
}
void WriteEnvironmentC(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("C");
    ++context.GetEnvironmentForWrite().mFrameIndex;
}
void ReadEnvironment(SystemContext& context) {
    context.GetData<Trace>()->mOrder.emplace_back("R");
    (void)context.GetEnvironment().mFrameIndex;
}

void Probe(SystemContext& context) {
    auto* const trace       = context.GetData<Trace>();
    trace->mCanReadPhysics  = context.CanReadResource(trace->mPhysics);
    trace->mCanWritePhysics = context.CanWriteResource(trace->mPhysics);
    trace->mCanReadRng      = context.CanReadResource(trace->mRng);
}

struct Integration {
    ComponentTypeIndex mPosition;
    ResourceTypeIndex mPhysics;
};

void StepPhysics(SystemContext& context) {
    const auto* const data = context.GetData<Integration>();
    ++context.GetResourceForWrite<PhysicsWorld>(data->mPhysics)->mSteps;
}

/// Position.y = gravity * steps: reads the resource the step wrote, in the same phase.
void ApplyPhysics(SystemContext& context) {
    const auto* const data    = context.GetData<Integration>();
    const auto* const physics = context.GetResource<PhysicsWorld>(data->mPhysics);
    context.ForEachChunk([&](ChunkView& view) {
        for (Position& position : context.Write<Position>(view, data->mPosition)) {
            position.mY = physics->mGravity * static_cast<float>(physics->mSteps);
        }
    });
}

struct WindowEdits {
    ResourceTypeIndex mPhysics;
};

void EditResources(FirewallWindow& window) {
    const auto* const edits                                             = window.GetData<WindowEdits>();
    window.GetResourceForWrite<PhysicsWorld>(edits->mPhysics)->mGravity = -1.0f;
    window.GetEnvironmentForWrite().mFrameIndex                         = 7;
    CHECK(window.GetResource<PhysicsWorld>(edits->mPhysics)->mSteps == 0);
    CHECK(window.GetEnvironment().mFrameIndex == 7);
}

/// A managed object that reads the Environment and rolls the Rng.
class Gambler final : public ManagedObject {
public:
    explicit Gambler(ResourceTypeIndex rng) noexcept : mRng(rng) {}

    void Update(ManagedContext& context) override {
        mSeenFrame            = context.GetEnvironment().mFrameIndex;
        Rng* const rng        = context.GetResourceForWrite<Rng>(mRng);
        rng->mState           = (rng->mState * 6364136223846793005ull) + 1442695040888963407ull;
        mLastRoll             = rng->mState;
        mSawRngThroughContext = context.GetResource<Rng>(mRng) == rng;
    }

    uint64_t mSeenFrame        = 0;
    uint64_t mLastRoll         = 0;
    bool mSawRngThroughContext = false;
    ResourceTypeIndex mRng;
};

} // namespace

TEST_CASE("the registry hands out indices by name and the Environment is resource 0", "[ecs][resource]") {
    SystemAllocator allocator;
    World world(allocator);
    ResourceRegistry& resources = world.GetResources();
    CHECK(resources.GetCount() == 1);
    CHECK(resources.Find(kEnvironmentResourceName) == kEnvironmentResource);
    CHECK(resources.GetInfo(kEnvironmentResource).mOrdering == ResourceOrdering::Automatic);
    CHECK(resources.GetAutomaticMask().Test(kEnvironmentResource));
    CHECK(resources.Get<Environment>(kEnvironmentResource) == &world.GetEnvironment());
    CHECK(world.GetEnvironment().mFixedStepSeconds == 1.0f / 30.0f);

    PhysicsWorld physics;
    const ResourceTypeIndex index = *resources.Register("test::PhysicsWorld", physics);
    CHECK(index.mValue == 1);
    CHECK(resources.GetInfo(index).mOrdering == ResourceOrdering::Explicit);
    CHECK(resources.GetInfo(index).mName == "test::PhysicsWorld");
    CHECK(resources.Find("test::PhysicsWorld") == index);
    CHECK(resources.Get<PhysicsWorld>(index) == &physics);
    CHECK_FALSE(resources.GetAutomaticMask().Test(index));
    CHECK(resources.Get(ResourceTypeIndex::Invalid()) == nullptr);
    CHECK_FALSE(resources.Find("test::Nothing").IsValid());

    PhysicsWorld other;
    CHECK(resources.Register("test::PhysicsWorld", other).error().mCode == ErrorCode::AlreadyExists);
    CHECK(resources.Register("", other).error().mCode == ErrorCode::InvalidArgument);
    CHECK(resources.Register("test::Null", nullptr, ResourceOrdering::Explicit).error().mCode ==
          ErrorCode::InvalidArgument);

    std::array<std::string, kMaxResourceTypes> names;
    for (uint32_t i = resources.GetCount(); i < kMaxResourceTypes; ++i) {
        names[i] = "test::Filler" + std::to_string(i);
        REQUIRE(resources.Register(names[i], other).has_value());
    }
    CHECK(resources.GetCount() == kMaxResourceTypes);
    CHECK(resources.Register("test::OneTooMany", other).error().mCode == ErrorCode::OutOfRange);
}

TEST_CASE("resource conflicts follow the component rules", "[ecs][resource][schedule]") {
    SECTION("two writers need an explicit order") {
        Fixture f;
        const SystemId a = *f.mSchedule.AddSystem(
            Describe("a", Phase::Variable, {}, {}, {}, ResourcesOf({f.mPhysicsResource}), &Nothing, nullptr));
        const SystemId b = *f.mSchedule.AddSystem(
            Describe("b", Phase::Variable, {}, {}, {}, ResourcesOf({f.mPhysicsResource}), &Nothing, nullptr));
        REQUIRE_FALSE(f.mSchedule.Build().has_value());
        CHECK(f.mSchedule.GetLastConflict().mFirst == a);
        CHECK(f.mSchedule.GetLastConflict().mSecond == b);
        REQUIRE(f.mSchedule.AddOrder(a, b).has_value());
        REQUIRE(f.mSchedule.Build().has_value());
    }
    SECTION("a writer and a reader need an explicit order") {
        Fixture f;
        (void)*f.mSchedule.AddSystem(
            Describe("reader", Phase::Variable, {}, {}, ResourcesOf({f.mPhysicsResource}), {}, &Nothing, nullptr));
        (void)*f.mSchedule.AddSystem(
            Describe("writer", Phase::Variable, {}, {}, {}, ResourcesOf({f.mPhysicsResource}), &Nothing, nullptr));
        REQUIRE_FALSE(f.mSchedule.Build().has_value());
    }
    SECTION("readers do not conflict, and the context reports the declared access") {
        Fixture f;
        Trace trace;
        trace.mPhysics = f.mPhysicsResource;
        trace.mRng     = f.mRngResource;
        (void)*f.mSchedule.AddSystem(
            Describe("reader", Phase::Variable, {}, {}, ResourcesOf({f.mPhysicsResource}), {}, &Nothing, nullptr));
        (void)*f.mSchedule.AddSystem(Describe("probe", Phase::Variable, {}, {}, ResourcesOf({f.mPhysicsResource}),
                                              ResourcesOf({f.mRngResource}), &Probe, &trace));
        REQUIRE(f.mSchedule.Build().has_value());
        REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
        CHECK(trace.mCanReadPhysics);
        CHECK_FALSE(trace.mCanWritePhysics);
        CHECK(trace.mCanReadRng); // a writer may read
    }
}

TEST_CASE("Environment writers are ordered automatically in registration order", "[ecs][resource][schedule]") {
    SECTION("writers and readers serialize without AddOrder; untouched systems stay free") {
        Fixture f;
        Trace trace;
        const ResourceMask environment = ResourcesOf({kEnvironmentResource});
        const SystemId a =
            *f.mSchedule.AddSystem(Describe("A", Phase::Variable, {}, {}, {}, environment, &WriteEnvironmentA, &trace));
        const SystemId r =
            *f.mSchedule.AddSystem(Describe("R", Phase::Variable, {}, {}, environment, {}, &ReadEnvironment, &trace));
        const SystemId b =
            *f.mSchedule.AddSystem(Describe("B", Phase::Variable, {}, {}, {}, environment, &WriteEnvironmentB, &trace));
        const SystemId free =
            *f.mSchedule.AddSystem(Describe("free", Phase::Variable, {}, {}, {}, {}, &Nothing, nullptr));
        const SystemId c =
            *f.mSchedule.AddSystem(Describe("C", Phase::Variable, {}, {}, {}, environment, &WriteEnvironmentC, &trace));
        REQUIRE(f.mSchedule.Build().has_value());
        CHECK(f.mSchedule.GetPredecessors(a).empty());
        CHECK(f.mSchedule.GetPredecessors(r).size() == 1);
        CHECK(f.mSchedule.GetPredecessors(r)[0] == a);
        CHECK(f.mSchedule.GetPredecessors(b).size() == 1);
        CHECK(f.mSchedule.GetPredecessors(b)[0] == r);
        CHECK(f.mSchedule.GetPredecessors(c).size() == 1);
        CHECK(f.mSchedule.GetPredecessors(c)[0] == b);
        CHECK(f.mSchedule.GetPredecessors(free).empty());

        JobSystem jobs(f.mAllocator, kThreeWorkers);
        for (int round = 0; round < 20; ++round) {
            trace.mOrder.clear();
            REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
            CHECK(trace.mOrder == std::vector<std::string_view>{"A", "R", "B", "C"});
        }
        CHECK(f.mWorld.GetEnvironment().mFrameIndex == 60);
    }
    SECTION("an explicit edge against registration order wins") {
        Fixture f;
        Trace trace;
        const ResourceMask environment = ResourcesOf({kEnvironmentResource});
        const SystemId a =
            *f.mSchedule.AddSystem(Describe("A", Phase::Variable, {}, {}, {}, environment, &WriteEnvironmentA, &trace));
        const SystemId b =
            *f.mSchedule.AddSystem(Describe("B", Phase::Variable, {}, {}, {}, environment, &WriteEnvironmentB, &trace));
        REQUIRE(f.mSchedule.AddOrder(b, a).has_value());
        REQUIRE(f.mSchedule.Build().has_value());
        CHECK(f.mSchedule.GetPredecessors(b).empty());
        CHECK(f.mSchedule.GetPredecessors(a).size() == 1);
        REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
        CHECK(trace.mOrder == std::vector<std::string_view>{"B", "A"});
    }
    SECTION("an Explicit resource is not ordered for free even beside the Environment") {
        Fixture f;
        const ResourceMask both = ResourcesOf({kEnvironmentResource, f.mPhysicsResource});
        (void)*f.mSchedule.AddSystem(Describe("a", Phase::Variable, {}, {}, {}, both, &Nothing, nullptr));
        (void)*f.mSchedule.AddSystem(
            Describe("b", Phase::Variable, {}, {}, {}, ResourcesOf({f.mPhysicsResource}), &Nothing, nullptr));
        // a and b conflict on the physics world only; the Environment edge does not cover that.
        REQUIRE_FALSE(f.mSchedule.Build().has_value());
    }
}

TEST_CASE("a resource stepped by one system and read by another matches the serial run",
          "[ecs][resource][schedule][job]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        for (uint32_t i = 0; i < 300; ++i) {
            (void)*f.mWorld.CreateEntity(MaskOf({f.mPosition}));
        }
        Integration integration{.mPosition = f.mPosition, .mPhysics = f.mPhysicsResource};
        const SystemId step = *f.mSchedule.AddSystem(Describe(
            "step", Phase::FixedSimulation, {}, {}, {}, ResourcesOf({f.mPhysicsResource}), &StepPhysics, &integration));
        const SystemId apply =
            *f.mSchedule.AddSystem(Describe("apply", Phase::FixedSimulation, {}, MaskOf({f.mPosition}),
                                            ResourcesOf({f.mPhysicsResource}), {}, &ApplyPhysics, &integration));
        REQUIRE(f.mSchedule.AddOrder(step, apply).has_value());
        REQUIRE(f.mSchedule.Build().has_value());
        if (parallel) {
            JobSystem jobs(f.mAllocator, kThreeWorkers);
            for (int frame = 0; frame < 5; ++frame) {
                REQUIRE(f.mSchedule.RunPhase(Phase::FixedSimulation, jobs).has_value());
            }
        } else {
            for (int frame = 0; frame < 5; ++frame) {
                REQUIRE(f.mSchedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
            }
        }
        CHECK(f.mPhysics.mSteps == 5);
        std::vector<float> state;
        f.mWorld.ForEachChunk(MaskOf({f.mPosition}), ComponentMask{}, [&](ChunkView& view) {
            for (const Position& position : view.Read<Position>(f.mPosition)) {
                state.push_back(position.mY);
            }
        });
        return state;
    };
    const auto serial   = simulate(false);
    const auto parallel = simulate(true);
    CHECK(serial.size() == 300);
    CHECK(serial == parallel);
    CHECK(serial[0] == -9.8f * 5.0f);
}

TEST_CASE("windows and managed objects reach resources through their contexts", "[ecs][resource][firewall][managed]") {
    Fixture f;
    WindowEdits edits{.mPhysics = f.mPhysicsResource};
    REQUIRE(f.mSchedule.RunWindow(&EditResources, &edits).has_value());
    CHECK(f.mPhysics.mGravity == -1.0f);
    CHECK(f.mWorld.GetEnvironment().mFrameIndex == 7);

    ManagedPool pool(f.mAllocator);
    const EntityId entity                           = *f.mWorld.CreateEntity(MaskOf({f.mBrainRef}));
    const ManagedId id                              = *pool.Create<Gambler>(entity, f.mRngResource);
    f.mWorld.GetForWrite<BrainRef>(entity)->mObject = id;
    ManagedSystem gamblers(pool, f.mBrainRef, nullptr);
    REQUIRE(f.mSchedule
                .AddSystem(
                    gamblers.Describe(Describe("gamblers", Phase::Variable, {}, {}, ResourcesOf({kEnvironmentResource}),
                                               ResourcesOf({f.mRngResource}), nullptr, nullptr)))
                .has_value());
    REQUIRE(f.mSchedule.Build().has_value());
    f.mWorld.GetEnvironmentForWrite().mFrameIndex = 42;
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    const auto* const gambler = pool.Resolve<Gambler>(id);
    CHECK(gambler->mSeenFrame == 42);
    CHECK(gambler->mLastRoll == f.mRng.mState);
    CHECK(gambler->mSawRngThroughContext);
    CHECK(f.mRng.mState != 1);
}
