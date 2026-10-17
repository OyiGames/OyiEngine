#include <oyi_core/Expected.h>
#include <oyi_core/Schema.h>
#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Managed.h>
#include <oyi_ecs/ManagedObject.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Schedule.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs/WorldFile.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_job/JobSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
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
using oyi::ecs::CommandBuffer;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::FirewallWindow;
using oyi::ecs::ManagedContext;
using oyi::ecs::ManagedId;
using oyi::ecs::ManagedObject;
using oyi::ecs::ManagedPool;
using oyi::ecs::ManagedSystem;
using oyi::ecs::ManagedTypeInfo;
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
    return SystemDesc{.mName           = name,
                      .mPhase          = phase,
                      .mRead           = read,
                      .mWrite          = write,
                      .mExclude        = ComponentMask{},
                      .mOptional       = ComponentMask{},
                      .mSends          = MessageMask{},
                      .mReceives       = MessageMask{},
                      .mReadResources  = oyi::ecs::ResourceMask{},
                      .mWriteResources = oyi::ecs::ResourceMask{},
                      .mMainThread     = false,
                      .mFunction       = nullptr,
                      .mData           = nullptr};
}

/// A managed "brain": reads its owner's previous Position, counts updates, drains Health. Its
/// state block (ADR-0024) is what tools see and world files keep; the rest is run-time only.
class Brain final : public ManagedObject {
    OYI_MANAGED(Brain, BrainState, oyi::ecs::Access<oyi::ecs::Read<Position>, oyi::ecs::Write<Health>>)
public:
    Brain() = default;
    Brain(ComponentTypeIndex position, ComponentTypeIndex health, std::vector<EntityId>* order) noexcept
        : mPosition(position), mHealth(health), mOrder(order) {}

    void Update(ManagedContext& context) override {
        ++mState.mUpdates;
        mRanOnMain = std::this_thread::get_id() == mMainThread;
        if (mOrder != nullptr) {
            mOrder->push_back(context.GetOwner());
        }
        const auto* const prev = context.Prev<Position>(mPosition);
        mState.mSawPrevX       = prev != nullptr ? prev->mX : -1.0f;
        if (auto* const health = context.LookupForWrite<Health>(mHealth, context.GetOwner()); health != nullptr) {
            health->mCurrent = static_cast<uint16_t>(health->mCurrent > 0 ? health->mCurrent - 1 : 0);
        }
    }

    /// Rebuilt from a world file: the type indices come from the loaded world's registry.
    void OnLoaded(World& world) override {
        mLoaded     = true;
        mPosition   = world.GetComponents().Find(oyi::core::ComponentTraits<Position>::kInfo.mTypeId);
        mHealth     = world.GetComponents().Find(oyi::core::ComponentTraits<Health>::kInfo.mTypeId);
        mMainThread = std::this_thread::get_id();
    }

    // Probes the tests read: public on purpose.
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    bool mRanOnMain = false;
    bool mLoaded    = false;
    std::thread::id mMainThread{std::this_thread::get_id()};
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mHealth;
    std::vector<EntityId>* mOrder = nullptr;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

/// A managed object without OYI_MANAGED: run-time only, never saved, invisible to tools.
class Scribble final : public ManagedObject {
public:
    void Update(ManagedContext& /*context*/) override { ++mUpdates; }
    uint32_t mUpdates = 0;
};

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    ComponentTypeIndex mBrainRef = *mWorld.GetComponents().Register<BrainRef>();
    ManagedPool& mPool           = mWorld.GetManaged();
    Snapshot mPrev{mAllocator};
    Schedule mSchedule{mAllocator, mWorld};

    Fixture() { REQUIRE(mWorld.RegisterManaged<Brain>().has_value()); }

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
    CHECK(f.BrainOf(first)->GetState().mSawPrevX == -1.0f);
    REQUIRE(f.mSchedule.RunPhase(Phase::FixedSimulation, jobs).has_value());
    REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
    CHECK(brains.GetLastUpdateCount() == 2);
    CHECK(f.BrainOf(first)->GetState().mUpdates == 2);
    CHECK(f.BrainOf(first)->mRanOnMain);
    CHECK(f.BrainOf(first)->GetState().mSawPrevX == 1.0f);
    CHECK(f.BrainOf(second)->GetState().mSawPrevX == 2.0f);
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
    CHECK(f.BrainOf(second)->GetState().mUpdates == 2);

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
                state.push_back(f.mPool.Resolve<Brain>(refs[i].mObject)->GetState().mSawPrevX);
            }
        });
        return state;
    };
    const auto serial   = simulate(false);
    const auto parallel = simulate(true);
    CHECK(serial.size() == 900);
    CHECK(serial == parallel);
}

namespace {

/// The managed tests' components in registration order, for a second world that loads a file.
struct LoadedTypes {
    ComponentTypeIndex mPosition;
    ComponentTypeIndex mVelocity;
    ComponentTypeIndex mHealth;
    ComponentTypeIndex mBrainRef;
};

[[nodiscard]] LoadedTypes RegisterComponents(World& world) {
    return LoadedTypes{.mPosition = *world.GetComponents().Register<Position>(),
                       .mVelocity = *world.GetComponents().Register<Velocity>(),
                       .mHealth   = *world.GetComponents().Register<Health>(),
                       .mBrainRef = *world.GetComponents().Register<BrainRef>()};
}

struct StateEdit {
    Fixture* mFixture = nullptr;
    EntityId mEntity;
    ManagedId mSeen;
    uint32_t mSawUpdates = 0;
    bool mInfoMatches    = false;
};

/// Reads the owner's state block through the window and writes a new one back.
void EditState(FirewallWindow& window) {
    auto* const self        = window.GetData<StateEdit>();
    self->mSeen             = window.FindManaged(self->mEntity, Brain::GetManagedTypeInfo().GetTypeId());
    self->mInfoMatches      = window.GetManagedInfo(self->mSeen) == &Brain::GetManagedTypeInfo();
    const auto* const state = window.GetManagedState<BrainState>(self->mSeen);
    self->mSawUpdates       = state != nullptr ? state->mUpdates : 999;
    window.WriteManagedState(self->mSeen, BrainState{.mUpdates = 40, .mSawPrevX = 4.0f});
    CHECK(window.GetManagedState<BrainState>(self->mSeen)->mUpdates == self->mSawUpdates); // not yet
    CHECK(window.GetManagedState(ManagedId{}).empty());
    CHECK(window.GetManagedInfo(ManagedId{}) == nullptr);
    CHECK_FALSE(window.FindManaged(EntityId{}, Brain::GetManagedTypeInfo().GetTypeId()).IsValid());
}

} // namespace

TEST_CASE("an OYI_MANAGED type is described by its state block and its access sets", "[ecs][managed][object]") {
    Fixture f;
    const ManagedTypeInfo& info = Brain::GetManagedTypeInfo();
    CHECK(info.GetName() == "oyi::ecs_fixture::BrainState");
    CHECK(info.GetStateSize() == sizeof(BrainState));
    CHECK(info.GetTypeId() == oyi::core::ComponentTraits<BrainState>::kInfo.mTypeId);
    REQUIRE(info.mReads.size() == 1);
    CHECK(info.mReads[0] == &oyi::core::ComponentTraits<Position>::kInfo);
    REQUIRE(info.mWrites.size() == 1);
    CHECK(info.mWrites[0] == &oyi::core::ComponentTraits<Health>::kInfo);
    CHECK(info.mReceives.empty());
    CHECK(info.mSends.empty());
    CHECK(info.mCreate != nullptr);

    // The world's registry: by id, by name, idempotent.
    CHECK(f.mWorld.GetManagedTypes().Find(info.GetTypeId()) == &info);
    CHECK(f.mWorld.GetManagedTypes().Find("oyi::ecs_fixture::BrainState") == &info);
    CHECK(f.mWorld.GetManagedTypes().Find("nope") == nullptr);
    REQUIRE(f.mWorld.RegisterManaged<Brain>().has_value());
    CHECK(f.mWorld.GetManagedTypes().GetCount() == 1);

    // Objects answer with their info; an untyped object has none and is never found by type.
    const EntityId entity = f.Spawn();
    CHECK(f.BrainOf(entity)->GetTypeInfo() == &info);
    CHECK(f.BrainOf(entity)->GetStateBytes() == &f.BrainOf(entity)->GetState());
    CHECK(f.mPool.FindByOwner(entity, info.GetTypeId()) == f.BrainOf(entity));
    const ManagedId scribble = *f.mPool.Create<Scribble>(entity);
    CHECK(f.mPool.Resolve(scribble)->GetTypeInfo() == nullptr);
    CHECK(f.mPool.FindByOwner(entity, oyi::core::ComponentTraits<Velocity>::kInfo.mTypeId) == nullptr);

    // The access sets resolve against the world's registries...
    SystemDesc desc = Access("brains", Phase::Variable, {}, {});
    REQUIRE(oyi::ecs::ApplyManagedAccess(info, f.mWorld, desc).has_value());
    CHECK(desc.mRead.Test(f.mPosition));
    CHECK(desc.mWrite.Test(f.mHealth));
    CHECK_FALSE(desc.mRead.Test(f.mHealth));
    ManagedSystem brains(f.mPool, f.mBrainRef, nullptr);
    const auto described = brains.Describe(Access("brains", Phase::Variable, {}, {}), info, f.mWorld);
    REQUIRE(described.has_value());
    CHECK(described->mMainThread);
    CHECK(described->mRead.Test(f.mBrainRef));
    CHECK(described->mRead.Test(f.mPosition));
    CHECK(described->mWrite.Test(f.mHealth));

    // ...and a world that lacks one of them refuses, leaving the description untouched.
    World bare(f.mAllocator);
    const ComponentTypeIndex barePosition = *bare.GetComponents().Register<Position>();
    SystemDesc untouched                  = Access("brains", Phase::Variable, {}, {});
    CHECK(oyi::ecs::ApplyManagedAccess(info, bare, untouched).error().mCode == ErrorCode::NotFound);
    CHECK_FALSE(untouched.mRead.Test(barePosition));
}

TEST_CASE("a managed state write replays at the sync point like any other command",
          "[ecs][managed][commands][object]") {
    Fixture f;
    const EntityId entity = f.Spawn();
    const EntityId other  = f.Spawn();
    const ManagedId id    = f.mWorld.Get<BrainRef>(entity)->mObject;
    const BrainState state{.mUpdates = 5, .mSawPrevX = 2.0f};

    CommandBuffer commands(f.mAllocator);
    commands.WriteManagedState(entity, id, &state, sizeof(state));
    commands.WriteManagedState(entity, id, &state, 3);                                        // wrong size
    commands.WriteManagedState(other, id, &state, sizeof(state));                             // wrong owner
    commands.WriteManagedState(entity, ManagedId{.mIndex = 99, .mGeneration = 1}, &state, 8); // stale id
    CHECK(f.BrainOf(entity)->GetState().mUpdates == 0);

    const std::array<CommandBuffer*, 1> buffers{&commands};
    const World::ReplayStats stats = f.mWorld.ReplayCommands(std::span<CommandBuffer* const>{buffers});
    CHECK(stats.mApplied == 1);
    CHECK(stats.mSkipped == 3);
    CHECK(f.BrainOf(entity)->GetState().mUpdates == 5);
    CHECK(f.BrainOf(entity)->GetState().mSawPrevX == 2.0f);
    CHECK(f.BrainOf(other)->GetState().mUpdates == 0);
}

TEST_CASE("a window reads state blocks and its writes land when it closes", "[ecs][managed][firewall][object]") {
    Fixture f;
    const EntityId entity                          = f.Spawn();
    f.BrainOf(entity)->GetStateForWrite().mUpdates = 3;
    StateEdit edit;
    edit.mFixture = &f;
    edit.mEntity  = entity;
    REQUIRE(f.mSchedule.RunWindow(&EditState, &edit).has_value());
    CHECK(edit.mSeen == f.mWorld.Get<BrainRef>(entity)->mObject);
    CHECK(edit.mInfoMatches);
    CHECK(edit.mSawUpdates == 3);
    CHECK(f.BrainOf(entity)->GetState().mUpdates == 40);
    CHECK(f.BrainOf(entity)->GetState().mSawPrevX == 4.0f);
}

TEST_CASE("managed objects and their state blocks survive the world file", "[ecs][managed][serialization][object]") {
    Fixture f;
    const EntityId first                           = f.Spawn();
    const EntityId second                          = f.Spawn();
    f.BrainOf(first)->GetStateForWrite().mUpdates  = 7;
    f.BrainOf(first)->GetStateForWrite().mSawPrevX = 1.5f;
    f.BrainOf(second)->GetStateForWrite().mUpdates = 9;
    const ManagedId scribble                       = *f.mPool.Create<Scribble>(first); // run-time only: not saved
    const ManagedId firstBrain                     = f.mWorld.Get<BrainRef>(first)->mObject;
    std::pmr::vector<std::byte> bytes;
    REQUIRE(f.mWorld.Save(bytes).has_value());

    oyi::ecs::WorldFileSummary summary(f.mAllocator);
    REQUIRE(oyi::ecs::DescribeSavedWorld(bytes, summary).has_value());
    CHECK(summary.mManagedCount == 2);
    CHECK(summary.mEntityCount == 2);

    // A world that knows the type rebuilds the objects at their ids, restores the state blocks
    // and lets OnLoaded fill in the rest; the pool's generations follow the file.
    World loaded(f.mAllocator);
    const LoadedTypes types = RegisterComponents(loaded);
    REQUIRE(loaded.RegisterManaged<Brain>().has_value());
    REQUIRE(loaded.Load(bytes).has_value());
    CHECK(loaded.GetManaged().GetLiveCount() == 2);
    CHECK(loaded.GetManaged().Resolve(scribble) == nullptr);
    auto* const brain = loaded.GetManaged().Resolve<Brain>(loaded.Get<BrainRef>(first)->mObject);
    REQUIRE(brain != nullptr);
    CHECK(brain->GetId() == firstBrain);
    CHECK(brain->GetOwner() == first);
    CHECK(brain->mLoaded);
    CHECK(brain->GetState().mUpdates == 7);
    CHECK(brain->GetState().mSawPrevX == 1.5f);
    CHECK(brain->mPosition == types.mPosition);
    CHECK(brain->mHealth == types.mHealth);
    const auto* const other = loaded.GetManaged().FindByOwner(second, Brain::GetManagedTypeInfo().GetTypeId());
    REQUIRE(other != nullptr);
    CHECK(loaded.GetManaged().Resolve<Brain>(other->GetId())->GetState().mUpdates == 9);

    // The restored objects run like the originals.
    Schedule schedule(f.mAllocator, loaded);
    ManagedSystem brains(loaded.GetManaged(), types.mBrainRef, nullptr);
    REQUIRE(
        schedule
            .AddSystem(*brains.Describe(Access("brains", Phase::Variable, {}, {}), Brain::GetManagedTypeInfo(), loaded))
            .has_value());
    REQUIRE(schedule.Build().has_value());
    REQUIRE(schedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(brains.GetLastUpdateCount() == 2);
    CHECK(brain->GetState().mUpdates == 8);
    CHECK(loaded.Get<Health>(first)->mCurrent == 99);

    // New objects after a load never collide with restored ids.
    const EntityId third = *loaded.CreateEntity(ComponentMask{});
    const ManagedId born = *loaded.GetManaged().Create<Brain>(third);
    CHECK(born.mIndex >= 2);
    CHECK(loaded.GetManaged().GetLiveCount() == 3);

    // A world that does not know the type refuses the file and stays empty.
    World ignorant(f.mAllocator);
    (void)RegisterComponents(ignorant);
    CHECK(ignorant.Load(bytes).error().mCode == ErrorCode::CorruptData);
    CHECK(ignorant.GetEntityCount() == 0);
    CHECK(ignorant.GetManaged().GetLiveCount() == 0);

    // Clearing a loaded world drops its objects with everything else.
    loaded.Clear();
    CHECK(loaded.GetManaged().GetLiveCount() == 0);
    CHECK(loaded.GetManaged().GetSlotCount() == 0);
}
