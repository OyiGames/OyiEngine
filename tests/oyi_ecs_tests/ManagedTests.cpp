#include <oyi_core/Expected.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_job/JobSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::ManagedContext;
using oyi::ecs::ManagedId;
using oyi::ecs::ManagedObject;
using oyi::ecs::ManagedPool;
using oyi::ecs::ManagedSystem;
using oyi::ecs::MessageMask;
using oyi::ecs::Phase;
using oyi::ecs::Schedule;
using oyi::ecs::Snapshot;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::World;
using oyi::job::JobSystem;
using namespace oyi::ecs_fixture;

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

[[nodiscard]] SystemDesc Access(std::string_view name, Phase phase, ComponentMask read, ComponentMask write) {
    return SystemDesc{.mName       = name,
                      .mPhase      = phase,
                      .mRead       = read,
                      .mWrite      = write,
                      .mExclude    = ComponentMask{},
                      .mOptional   = ComponentMask{},
                      .mSends      = MessageMask{},
                      .mReceives   = MessageMask{},
                      .mMainThread = false,
                      .mFunction   = nullptr,
                      .mData       = nullptr};
}

/// A managed "brain": reads its owner's previous Position, counts updates, drains Health.
class Brain final : public ManagedObject {
public:
    Brain(ComponentTypeIndex position, ComponentTypeIndex health, std::vector<EntityId>* order) noexcept
        : mPosition(position), mHealth(health), mOrder(order) {}

    void Update(ManagedContext& context) override {
        ++mUpdates;
        mRanOnMain = std::this_thread::get_id() == mMainThread;
        if (mOrder != nullptr) {
            mOrder->push_back(context.GetOwner());
        }
        const auto* const prev = context.Prev<Position>(mPosition);
        mSawPrevX              = prev != nullptr ? prev->mX : -1.0f;
        if (auto* const health = context.LookupForWrite<Health>(mHealth, context.GetOwner()); health != nullptr) {
            health->mCurrent = static_cast<uint16_t>(health->mCurrent > 0 ? health->mCurrent - 1 : 0);
        }
    }

    uint32_t mUpdates = 0;
    bool mRanOnMain   = false;
    float mSawPrevX   = -2.0f;
    std::thread::id mMainThread{std::this_thread::get_id()};
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mHealth;
    std::vector<EntityId>* mOrder;
};

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    ComponentTypeIndex mBrainRef = *mWorld.GetComponents().Register<BrainRef>();
    ManagedPool mPool{mAllocator};
    Snapshot mPrev{mAllocator};
    Schedule mSchedule{mAllocator, mWorld};

    /// An entity with Position, Health and a Brain bound to it.
    [[nodiscard]] EntityId Spawn(std::vector<EntityId>* order = nullptr) {
        const EntityId entity                         = *mWorld.CreateEntity(MaskOf({mPosition, mHealth, mBrainRef}));
        const ManagedId brain                         = *mPool.Create<Brain>(entity, mPosition, mHealth, order);
        mWorld.GetForWrite<BrainRef>(entity)->mObject = brain;
        return entity;
    }

    [[nodiscard]] Brain* BrainOf(EntityId entity) {
        return mPool.Resolve<Brain>(mWorld.Get<BrainRef>(entity)->mObject);
    }
};

} // namespace

TEST_CASE("a pool creates, resolves and destroys objects with generation-checked ids", "[ecs][managed]") {
    SystemAllocator allocator;
    World world(allocator);
    ManagedPool pool(allocator);
    const EntityId owner = *world.CreateEntity(ComponentMask{});

    const ManagedId id = *pool.Create<Brain>(owner, ComponentTypeIndex{}, ComponentTypeIndex{}, nullptr);
    REQUIRE(id.IsValid());
    CHECK(pool.IsAlive(id));
    CHECK(pool.GetLiveCount() == 1);
    REQUIRE(pool.Resolve(id) != nullptr);
    CHECK(pool.Resolve(id)->GetOwner() == owner);
    CHECK(pool.Resolve(id)->GetId() == id);
    CHECK(allocator.GetStats().mLiveAllocations > 0);

    pool.Destroy(id);
    CHECK_FALSE(pool.IsAlive(id));
    CHECK(pool.Resolve(id) == nullptr);
    CHECK(pool.GetLiveCount() == 0);
    pool.Destroy(id); // idempotent

    const ManagedId reused = *pool.Create<Brain>(owner, ComponentTypeIndex{}, ComponentTypeIndex{}, nullptr);
    CHECK(reused.mIndex == id.mIndex);
    CHECK(reused.mGeneration != id.mGeneration);
    CHECK_FALSE(pool.IsAlive(id));
    CHECK(pool.IsAlive(reused));
}

TEST_CASE("objects of dead owners are collected", "[ecs][managed]") {
    SystemAllocator allocator;
    World world(allocator);
    ManagedPool pool(allocator);
    const EntityId a       = *world.CreateEntity(ComponentMask{});
    const EntityId b       = *world.CreateEntity(ComponentMask{});
    const ManagedId brainA = *pool.Create<Brain>(a, ComponentTypeIndex{}, ComponentTypeIndex{}, nullptr);
    const ManagedId brainB = *pool.Create<Brain>(b, ComponentTypeIndex{}, ComponentTypeIndex{}, nullptr);
    CHECK(pool.CollectDead(world) == 0);

    REQUIRE(world.DestroyEntity(a).has_value());
    CHECK(pool.CollectDead(world) == 1);
    CHECK_FALSE(pool.IsAlive(brainA));
    CHECK(pool.IsAlive(brainB));
    uint32_t seen = 0;
    pool.ForEach([&](ManagedObject& object) {
        CHECK(object.GetOwner() == b);
        ++seen;
    });
    CHECK(seen == 1);
}

TEST_CASE("a managed system updates every live object on the main thread with the Prev snapshot",
          "[ecs][managed][schedule]") {
    Fixture f;
    std::vector<EntityId> order;
    const EntityId first                       = f.Spawn(&order);
    const EntityId second                      = f.Spawn(&order);
    f.mWorld.GetForWrite<Position>(first)->mX  = 1.0f;
    f.mWorld.GetForWrite<Position>(second)->mX = 2.0f;

    ManagedSystem brains(f.mPool, f.mBrainRef, &f.mPrev);
    REQUIRE(
        f.mSchedule
            .AddSystem(brains.Describe(Access("brains", Phase::Variable, MaskOf({f.mPosition}), MaskOf({f.mHealth}))))
            .has_value());
    REQUIRE(f.mSchedule.Build().has_value());
    CHECK(f.mSchedule.GetDesc(f.mSchedule.GetOrder(Phase::Variable)[0]).mMainThread);
    f.mSchedule.SetSnapshotCapture(&f.mPrev, MaskOf({f.mPosition}), Phase::FixedSimulation);

    JobSystem jobs(f.mAllocator,
                   JobSystem::Config{.mWorkerCount = 2, .mMaxJobs = 64, .mMaxCounters = 32, .mSpinIterations = 8});
    // No snapshot yet: Prev reads nothing. FixedSimulation captures; Variable updates.
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(f.BrainOf(first)->mSawPrevX == -1.0f);
    REQUIRE(f.mSchedule.RunPhase(Phase::FixedSimulation, jobs).has_value());
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(brains.GetLastUpdateCount() == 2);
    CHECK(f.BrainOf(first)->mUpdates == 2);
    CHECK(f.BrainOf(first)->mRanOnMain);
    CHECK(f.BrainOf(first)->mSawPrevX == 1.0f);
    CHECK(f.BrainOf(second)->mSawPrevX == 2.0f);
    CHECK(f.mWorld.Get<Health>(first)->mCurrent == 98);
    CHECK(order == std::vector<EntityId>{first, second, first, second});
}

TEST_CASE("a managed system collects the objects of destroyed owners before updating", "[ecs][managed][schedule]") {
    Fixture f;
    const EntityId first       = f.Spawn();
    const EntityId second      = f.Spawn();
    const ManagedId firstBrain = f.mWorld.Get<BrainRef>(first)->mObject;
    ManagedSystem brains(f.mPool, f.mBrainRef, nullptr);
    REQUIRE(
        f.mSchedule.AddSystem(brains.Describe(Access("brains", Phase::Variable, {}, MaskOf({f.mHealth})))).has_value());
    REQUIRE(f.mSchedule.Build().has_value());

    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(brains.GetLastUpdateCount() == 2);
    REQUIRE(f.mWorld.DestroyEntity(first).has_value());
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(brains.GetLastCollected() == 1);
    CHECK(brains.GetLastUpdateCount() == 1);
    CHECK_FALSE(f.mPool.IsAlive(firstBrain));
    CHECK(f.mPool.GetLiveCount() == 1);
    CHECK(f.BrainOf(second)->mUpdates == 2);

    // A dangling reference (object destroyed by hand) is skipped, not crashed on.
    f.mPool.Destroy(f.mWorld.Get<BrainRef>(second)->mObject);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(brains.GetLastUpdateCount() == 0);
}

TEST_CASE("managed updates beside parallel jobs match the serial run", "[ecs][managed][schedule][job]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        for (uint32_t i = 0; i < 300; ++i) {
            const EntityId entity                          = f.Spawn();
            f.mWorld.GetForWrite<Health>(entity)->mCurrent = static_cast<uint16_t>(3 + (i % 4));
            REQUIRE(f.mWorld.AddComponent(entity, f.mVelocity).has_value());
            f.mWorld.GetForWrite<Velocity>(entity)->mX = static_cast<float>(i % 3);
        }
        struct Types {
            ComponentTypeIndex mPosition;
            ComponentTypeIndex mVelocity;
        };
        Types types{.mPosition = f.mPosition, .mVelocity = f.mVelocity};
        const auto move = [](SystemContext& context) {
            const auto* const t = context.GetData<Types>();
            context.ForEachChunk([&](ChunkView& view) {
                const std::span<const Velocity> velocities = context.Read<Velocity>(view, t->mVelocity);
                const std::span<Position> positions        = context.Write<Position>(view, t->mPosition);
                for (size_t i = 0; i < positions.size(); ++i) {
                    positions[i].mX += velocities[i].mX;
                }
            });
        };
        SystemDesc moveDesc = Access("move", Phase::Variable, MaskOf({f.mVelocity}), MaskOf({f.mPosition}));
        moveDesc.mFunction  = move;
        moveDesc.mData      = &types;
        ManagedSystem brains(f.mPool, f.mBrainRef, &f.mPrev);
        const auto moveId  = *f.mSchedule.AddSystem(moveDesc);
        const auto brainId = *f.mSchedule.AddSystem(
            brains.Describe(Access("brains", Phase::Variable, MaskOf({f.mPosition}), MaskOf({f.mHealth}))));
        // Brains read Position that move writes: ordered after it.
        REQUIRE(f.mSchedule.AddOrder(moveId, brainId).has_value());
        REQUIRE(f.mSchedule.Build().has_value());
        f.mSchedule.SetSnapshotCapture(&f.mPrev, MaskOf({f.mPosition}), Phase::Variable);

        if (parallel) {
            JobSystem jobs(
                f.mAllocator,
                JobSystem::Config{.mWorkerCount = 3, .mMaxJobs = 64, .mMaxCounters = 32, .mSpinIterations = 8});
            for (int frame = 0; frame < 5; ++frame) {
                REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
            }
        } else {
            for (int frame = 0; frame < 5; ++frame) {
                REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
            }
        }
        std::vector<float> state;
        f.mWorld.ForEachChunk(MaskOf({f.mPosition, f.mHealth, f.mBrainRef}), ComponentMask{}, [&](ChunkView& view) {
            const std::span<const Position> positions = view.Read<Position>(f.mPosition);
            const std::span<const Health> healths     = view.Read<Health>(f.mHealth);
            const std::span<const BrainRef> refs      = view.Read<BrainRef>(f.mBrainRef);
            for (size_t i = 0; i < positions.size(); ++i) {
                state.push_back(positions[i].mX);
                state.push_back(static_cast<float>(healths[i].mCurrent));
                state.push_back(f.mPool.Resolve<Brain>(refs[i].mObject)->mSawPrevX);
            }
        });
        return state;
    };
    const auto serial   = simulate(false);
    const auto parallel = simulate(true);
    CHECK(serial.size() == 900);
    CHECK(serial == parallel);
}
