#include <oyi_core/Expected.h>
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
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_job/JobSystem.h>
#include <oyi_object/Components.gen.h> // IWYU pragma: keep
#include <oyi_object/Components.h>
#include <oyi_object/ObjectFactory.h>
#include <oyi_object/ObjectLayer.h>
#include <oyi_object/Prototype.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using oyi::core::ErrorCode;
using oyi::core::SystemAllocator;
using oyi::ecs::ChunkView;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::EntityMark;
using oyi::ecs::FirewallWindow;
using oyi::ecs::ManagedContext;
using oyi::ecs::ManagedObject;
using oyi::ecs::MessageMask;
using oyi::ecs::PendingEntity;
using oyi::ecs::Phase;
using oyi::ecs::Schedule;
using oyi::ecs::SystemContext;
using oyi::ecs::SystemDesc;
using oyi::ecs::World;
using oyi::job::JobSystem;
using oyi::object::ComponentValue;
using oyi::object::Despawning;
using oyi::object::ManagedBinding;
using oyi::object::ObjectFactory;
using oyi::object::ObjectInfo;
using oyi::object::ObjectLayer;
using oyi::object::ObjectTypes;
using oyi::object::ParentLink;
using oyi::object::PrototypeDesc;
using oyi::object::PrototypeId;
using oyi::object::RegisterObjectTypes;
using oyi::object::Spawned;
using oyi::object::SpawnOverrides;
using oyi::object::ValueOf;
using namespace oyi::ecs_fixture;

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

/// What the brains of a test saw; objects die with their owners, so the log lives here.
struct BrainLog {
    uint32_t mSpawned    = 0;
    uint32_t mDespawning = 0;
    uint32_t mBatchSeen  = 0; ///< objects alive when the last OnSpawned ran
    bool mDespawnOnSpawn = false;
    ObjectLayer* mLayer  = nullptr;
    ComponentTypeIndex mInfo;
};

BrainLog gLog; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/// A managed brain the door prototype instantiates: counts its lifecycle calls.
class Brain final : public ManagedObject {
    OYI_MANAGED(Brain, BrainState, oyi::ecs::Access<>)
public:
    void Update(ManagedContext& /*context*/) override { ++mState.mUpdates; }

    void OnSpawned(FirewallWindow& window) override {
        ++gLog.mSpawned;
        uint32_t seen = 0;
        window.ForEachEntity(MaskOf({gLog.mInfo}), ComponentMask{}, [&seen](EntityId) { ++seen; });
        gLog.mBatchSeen = seen;
        if (gLog.mDespawnOnSpawn) {
            ObjectFactory(*gLog.mLayer, window).Despawn(GetOwner());
        }
    }

    void OnDespawning(FirewallWindow& window) override {
        ++gLog.mDespawning;
        CHECK(window.IsAlive(GetOwner())); // still there for one sync point
    }
};

/// What a receiving system collected: (entity, prototype index) per message.
struct Seen {
    std::vector<std::pair<EntityId, uint32_t>> mSpawned;
    std::vector<std::pair<EntityId, uint32_t>> mDespawning;
    ObjectTypes mTypes;
    uint32_t mRuns = 0;
};

// The signature is SystemFunction's; the context is only read.
void ReceiverSystem(SystemContext& context) { // NOLINT(misc-const-correctness)
    auto* const seen = context.GetData<Seen>();
    ++seen->mRuns;
    const auto spawned = context.Received<Spawned>(seen->mTypes.mSpawned);
    for (size_t i = 0; i < spawned.mTargets.size(); ++i) {
        seen->mSpawned.emplace_back(spawned.mTargets[i], spawned.mPayloads[i].mPrototype.mIndex);
    }
    const auto despawning = context.Received<Despawning>(seen->mTypes.mDespawning);
    for (size_t i = 0; i < despawning.mTargets.size(); ++i) {
        seen->mDespawning.emplace_back(despawning.mTargets[i], despawning.mPayloads[i].mPrototype.mIndex);
    }
}

/// A system that spawns one door per run through the factory.
struct Spawner {
    ObjectLayer* mLayer = nullptr;
    PrototypeId mDoor;
    uint32_t mRuns = 0;
};

void SpawnerSystem(SystemContext& context) {
    auto* const self = context.GetData<Spawner>();
    ++self->mRuns;
    (void)ObjectFactory(*self->mLayer, context).Spawn(self->mDoor);
}

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    ComponentTypeIndex mBrainRef = *mWorld.GetComponents().Register<BrainRef>();
    ObjectTypes mTypes           = *RegisterObjectTypes(mWorld);
    ObjectLayer mLayer{mAllocator, mWorld, mTypes};
    Schedule mSchedule{mAllocator, mWorld};
    Seen mSeen{.mSpawned = {}, .mDespawning = {}, .mTypes = mTypes, .mRuns = 0};
    Position mDoorPosition{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f};
    Position mPanelPosition{.mX = 9.0f, .mY = 9.0f, .mZ = 9.0f};
    std::array<ComponentValue, 1> mDoorDefaults{ValueOf(mPosition, mDoorPosition)};
    std::array<ComponentValue, 1> mPanelDefaults{ValueOf(mPosition, mPanelPosition)};
    std::array<ManagedBinding, 1> mDoorManaged{
        ManagedBinding{.mType = &Brain::GetManagedTypeInfo(), .mReference = mBrainRef, .mOffset = 0}};
    /// The panel hangs under the door: Health::mLastAttacker stands in for a parent link.
    PrototypeDesc mPanel{.mName     = "door.panel",
                         .mMask     = MaskOf({mPosition, mHealth}),
                         .mDefaults = mPanelDefaults,
                         .mManaged  = {},
                         .mChildren = {}};
    std::array<const PrototypeDesc*, 1> mDoorChildren{&mPanel};
    PrototypeDesc mDoor{.mName     = "door",
                        .mMask     = MaskOf({mPosition, mHealth, mBrainRef}),
                        .mDefaults = mDoorDefaults,
                        .mManaged  = mDoorManaged,
                        .mChildren = mDoorChildren};

    Fixture() {
        gLog        = BrainLog{};
        gLog.mLayer = &mLayer;
        gLog.mInfo  = mTypes.mObjectInfo;
        REQUIRE(mLayer.Install(mSchedule).has_value());
        mLayer.SetParentLink(
            ParentLink{.mType = mHealth, .mOffset = static_cast<uint32_t>(offsetof(Health, mLastAttacker))});
        REQUIRE(mSchedule
                    .AddSystem(SystemDesc{.mName     = "receiver",
                                          .mPhase    = Phase::Variable,
                                          .mRead     = {},
                                          .mWrite    = {},
                                          .mExclude  = {},
                                          .mOptional = {},
                                          .mSends    = {},
                                          .mReceives = MessageMask{}.With(mTypes.mSpawned).With(mTypes.mDespawning),
                                          .mReadResources  = {},
                                          .mWriteResources = {},
                                          .mMainThread     = false,
                                          .mFunction       = &ReceiverSystem,
                                          .mData           = &mSeen})
                    .has_value());
    }

    [[nodiscard]] PrototypeId RegisterDoor() { return *mLayer.GetPrototypes().Register(mDoor); }

    /// The spawned (non-prototype) entities with ObjectInfo, in chunk order.
    [[nodiscard]] std::vector<EntityId> Objects() {
        std::vector<EntityId> entities;
        mWorld.ForEachChunk(MaskOf({mTypes.mObjectInfo}), ComponentMask{}, [&](ChunkView& view) {
            for (const EntityId entity : view.GetEntities()) {
                entities.push_back(entity);
            }
        });
        return entities;
    }

    [[nodiscard]] EntityId RootOf(std::span<const EntityId> entities) const {
        for (const EntityId entity : entities) {
            if (mWorld.Get<ObjectInfo>(entity)->mRoot == entity) {
                return entity;
            }
        }
        return EntityId::Invalid();
    }

    [[nodiscard]] Brain* BrainOf(EntityId entity) {
        return mWorld.GetManaged().Resolve<Brain>(mWorld.Get<BrainRef>(entity)->mObject);
    }
};

struct WindowSpawn {
    Fixture* mFixture = nullptr;
    PrototypeId mPrototype;
    Position mOverride{.mX = 5.0f, .mY = 0.0f, .mZ = 0.0f};
    EntityId mDespawn;
    PendingEntity mPending;
};

void SpawnInWindow(FirewallWindow& window) {
    auto* const self = window.GetData<WindowSpawn>();
    ObjectFactory factory(self->mFixture->mLayer, window);
    if (self->mPrototype.IsValid()) {
        const std::array<ComponentValue, 2> values{ValueOf(self->mFixture->mPosition, self->mOverride),
                                                   ValueOf(self->mFixture->mVelocity, Velocity{})}; // not in the mask
        self->mPending = factory.Spawn(self->mPrototype, SpawnOverrides{.mValues = values});
        CHECK(self->mPending.IsValid());
        CHECK_FALSE(factory.Spawn(PrototypeId{.mIndex = 77, .mGeneration = 1}).IsValid());
    }
    if (self->mDespawn.IsValid()) {
        factory.Despawn(self->mDespawn);
    }
}

} // namespace

TEST_CASE("prototypes register as prototype entities with their defaults, bindings and children",
          "[object][prototype]") {
    Fixture f;
    const PrototypeId door = f.RegisterDoor();
    REQUIRE(door.IsValid());
    CHECK(f.mLayer.GetPrototypes().GetCount() == 2);
    CHECK(f.mLayer.GetPrototypes().Find("door") == door);
    CHECK_FALSE(f.mLayer.GetPrototypes().Find("window").IsValid());
    const oyi::object::Prototype& prototype = f.mLayer.GetPrototypes().Get(door);
    CHECK(prototype.mName == "door");
    CHECK_FALSE(prototype.mParent.IsValid());
    REQUIRE(prototype.mChildren.size() == 1);
    const oyi::object::Prototype& panel = f.mLayer.GetPrototypes().Get(prototype.mChildren[0]);
    CHECK(panel.mName == "door.panel");
    CHECK(panel.mParent == door);
    CHECK(panel.mChildren.empty());
    CHECK(prototype.mManaged.size() == 1);
    CHECK(f.mWorld.GetManagedTypes().Find(Brain::GetManagedTypeInfo().GetTypeId()) == &Brain::GetManagedTypeInfo());

    // Prototype entities: marked, carrying the defaults and their ObjectInfo, outside queries.
    CHECK(f.mWorld.GetEntityCount() == 2);
    CHECK(f.mWorld.GetMark(prototype.mEntity) == EntityMark::Prototype);
    CHECK(f.mWorld.GetMark(panel.mEntity) == EntityMark::Prototype);
    CHECK(f.mWorld.Get<Position>(prototype.mEntity)->mY == 2.0f);
    CHECK(f.mWorld.Get<Position>(panel.mEntity)->mZ == 9.0f);
    CHECK(f.mWorld.Get<Health>(prototype.mEntity)->mCurrent == 100); // untouched default
    CHECK(f.mWorld.Get<ObjectInfo>(prototype.mEntity)->mPrototype == door);
    CHECK_FALSE(f.mWorld.Get<ObjectInfo>(prototype.mEntity)->mRoot.IsValid());
    CHECK(f.mWorld.HasComponent(prototype.mEntity, f.mBrainRef));
    CHECK(f.Objects().empty());

    // Refusals leave nothing behind.
    CHECK(f.mLayer.GetPrototypes().Register(f.mDoor).error().mCode == ErrorCode::AlreadyExists);
    const std::array<ManagedBinding, 1> badBinding{
        ManagedBinding{.mType = &Brain::GetManagedTypeInfo(), .mReference = f.mBrainRef, .mOffset = 0}};
    const PrototypeDesc noReference{.mName     = "bad.binding",
                                    .mMask     = MaskOf({f.mPosition}),
                                    .mDefaults = {},
                                    .mManaged  = badBinding,
                                    .mChildren = {}};
    CHECK(f.mLayer.GetPrototypes().Register(noReference).error().mCode == ErrorCode::InvalidArgument);
    const std::array<ComponentValue, 1> badDefault{
        ComponentValue{.mType = f.mPosition, .mData = &f.mDoorPosition, .mSize = 3}};
    const PrototypeDesc wrongSize{.mName     = "bad.default",
                                  .mMask     = MaskOf({f.mPosition}),
                                  .mDefaults = badDefault,
                                  .mManaged  = {},
                                  .mChildren = {}};
    CHECK(f.mLayer.GetPrototypes().Register(wrongSize).error().mCode == ErrorCode::InvalidArgument);
    const PrototypeDesc unnamed{.mName = "", .mMask = {}, .mDefaults = {}, .mManaged = {}, .mChildren = {}};
    CHECK(f.mLayer.GetPrototypes().Register(unnamed).error().mCode == ErrorCode::InvalidArgument);
    CHECK(f.mLayer.GetPrototypes().GetCount() == 2);
    CHECK(f.mWorld.GetEntityCount() == 2);
}

TEST_CASE("a spawn materialises at the sync point with its objects, links, overrides and Spawned",
          "[object][factory]") {
    Fixture f;
    const PrototypeId door = f.RegisterDoor();
    REQUIRE(f.mSchedule.Build().has_value());
    WindowSpawn spawn{.mFixture = &f, .mPrototype = door, .mDespawn = EntityId::Invalid(), .mPending = {}};
    REQUIRE(f.mSchedule.RunWindow(&SpawnInWindow, &spawn).has_value());

    // Two entities: the door (root) and its panel, both locked, both pointing at the root.
    const std::vector<EntityId> objects = f.Objects();
    REQUIRE(objects.size() == 2);
    const EntityId root = f.RootOf(objects);
    REQUIRE(root.IsValid());
    const EntityId panel = objects[0] == root ? objects[1] : objects[0];
    CHECK(f.mWorld.GetEntityCount() == 4);
    CHECK(f.mWorld.GetMark(root) == EntityMark::Locked);
    CHECK(f.mWorld.GetMark(panel) == EntityMark::Locked);
    CHECK(f.mWorld.Get<ObjectInfo>(root)->mPrototype == door);
    CHECK(f.mWorld.Get<ObjectInfo>(panel)->mPrototype == f.mLayer.GetPrototypes().Get(door).mChildren[0]);
    CHECK(f.mWorld.Get<ObjectInfo>(panel)->mRoot == root);
    CHECK(f.mWorld.Get<Health>(panel)->mLastAttacker == root); // the parent link
    CHECK_FALSE(f.mWorld.Get<Health>(root)->mLastAttacker.IsValid());
    // Overrides on the root only; the prototype keeps its values.
    CHECK(f.mWorld.Get<Position>(root)->mX == 5.0f);
    CHECK(f.mWorld.Get<Position>(root)->mY == 0.0f);
    CHECK(f.mWorld.Get<Position>(panel)->mX == 9.0f);
    CHECK(f.mWorld.Get<Position>(f.mLayer.GetPrototypes().Get(door).mEntity)->mX == 1.0f);
    CHECK_FALSE(f.mWorld.HasComponent(root, f.mVelocity));

    // The brain exists, ran OnSpawned once with the whole batch visible, and Spawned went out.
    REQUIRE(f.BrainOf(root) != nullptr);
    CHECK(f.BrainOf(root)->GetOwner() == root);
    CHECK(gLog.mSpawned == 1);
    CHECK(gLog.mBatchSeen == 2);
    CHECK(f.mWorld.GetManaged().GetLiveCount() == 1);
    CHECK(f.mLayer.GetLastHookStats().mSpawned == 2);
    CHECK(f.mSchedule.GetSyncHookRunCount() == 1);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    REQUIRE(f.mSeen.mSpawned.size() == 2);
    CHECK(f.mSeen.mSpawned[0] == std::pair{root, door.mIndex});
    CHECK(f.mSeen.mSpawned[1].first == panel);
    CHECK(f.mSeen.mDespawning.empty());
    CHECK(f.mSchedule.GetSyncHookRunCount() == 2); // the phase's sync point opened it too
    CHECK(f.mLayer.GetLastHookStats().mSpawned == 0);

    // Locked: the factory's rows keep their composition.
    CHECK(f.mWorld.AddComponent(root, f.mVelocity).error().mCode == ErrorCode::InvalidState);
}

TEST_CASE("systems spawn through the factory and the parallel run matches the serial one", "[object][factory][job]") {
    const auto simulate = [](bool parallel) {
        Fixture f;
        const PrototypeId door = f.RegisterDoor();
        Spawner spawner{.mLayer = &f.mLayer, .mDoor = door, .mRuns = 0};
        REQUIRE(f.mSchedule
                    .AddSystem(SystemDesc{.mName           = "spawner",
                                          .mPhase          = Phase::FixedSimulation,
                                          .mRead           = {},
                                          .mWrite          = {},
                                          .mExclude        = {},
                                          .mOptional       = {},
                                          .mSends          = {},
                                          .mReceives       = {},
                                          .mReadResources  = oyi::ecs::ResourceMask{}.With(f.mLayer.GetResourceType()),
                                          .mWriteResources = {},
                                          .mMainThread     = false,
                                          .mFunction       = &SpawnerSystem,
                                          .mData           = &spawner})
                    .has_value());
        REQUIRE(f.mSchedule.Build().has_value());
        JobSystem jobs(f.mAllocator,
                       JobSystem::Config{.mWorkerCount = 2, .mMaxJobs = 64, .mMaxCounters = 32, .mSpinIterations = 8});
        for (int frame = 0; frame < 3; ++frame) {
            if (parallel) {
                REQUIRE(f.mSchedule.RunPhase(Phase::FixedSimulation, jobs).has_value());
                REQUIRE(f.mSchedule.RunPhase(Phase::Variable, jobs).has_value());
            } else {
                REQUIRE(f.mSchedule.RunPhaseSerial(Phase::FixedSimulation).has_value());
                REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
            }
        }
        CHECK(f.Objects().size() == 6);
        CHECK(f.mWorld.GetManaged().GetLiveCount() == 3);
        CHECK(f.mSeen.mSpawned.size() == 6);
        CHECK(gLog.mSpawned == 3);
        return f.mWorld.ComputeStateHash();
    };
    CHECK(simulate(false) == simulate(true));
}

TEST_CASE("a despawn announces itself one sync point before the object and its entities die",
          "[object][factory][lifecycle]") {
    Fixture f;
    const PrototypeId door = f.RegisterDoor();
    REQUIRE(f.mSchedule.Build().has_value());
    WindowSpawn spawn{.mFixture = &f, .mPrototype = door, .mDespawn = EntityId::Invalid(), .mPending = {}};
    REQUIRE(f.mSchedule.RunWindow(&SpawnInWindow, &spawn).has_value());
    const std::vector<EntityId> objects = f.Objects();
    const EntityId root                 = f.RootOf(objects);
    const EntityId panel                = objects[0] == root ? objects[1] : objects[0];
    const oyi::ecs::ManagedId brain     = f.mWorld.Get<BrainRef>(root)->mObject;

    // Sync point 1: OnDespawning and Despawning; everything is still alive.
    WindowSpawn despawn{.mFixture = &f, .mPrototype = PrototypeId::Invalid(), .mDespawn = root, .mPending = {}};
    REQUIRE(f.mSchedule.RunWindow(&SpawnInWindow, &despawn).has_value());
    CHECK(gLog.mDespawning == 1);
    CHECK(f.mLayer.GetLastHookStats().mDespawning == 2);
    CHECK(f.mLayer.GetLastHookStats().mDestroyed == 0);
    CHECK(f.mWorld.IsAlive(root));
    CHECK(f.mWorld.IsAlive(panel));
    CHECK(f.mWorld.GetManaged().IsAlive(brain));

    // The receivers see Despawning with the entity alive; that phase's sync point then destroys.
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    REQUIRE(f.mSeen.mDespawning.size() == 2);
    CHECK(f.mSeen.mDespawning[0] == std::pair{root, door.mIndex});
    CHECK(f.mSeen.mDespawning[1].first == panel);
    CHECK(f.mLayer.GetLastHookStats().mDestroyed == 2);
    CHECK_FALSE(f.mWorld.IsAlive(root));
    CHECK_FALSE(f.mWorld.IsAlive(panel));
    CHECK_FALSE(f.mWorld.GetManaged().IsAlive(brain));
    CHECK(f.mWorld.GetManaged().GetLiveCount() == 0);
    CHECK(f.mWorld.GetEntityCount() == 2); // the prototypes

    // A second request for a dead entity, and one for a prototype-less entity, are harmless.
    WindowSpawn again{.mFixture = &f, .mPrototype = PrototypeId::Invalid(), .mDespawn = root, .mPending = {}};
    REQUIRE(f.mSchedule.RunWindow(&SpawnInWindow, &again).has_value());
    CHECK(f.mLayer.GetLastHookStats().mDespawning == 0);
}

TEST_CASE("despawning a child takes only that entity; despawning from OnSpawned waits a sync point",
          "[object][factory][lifecycle]") {
    Fixture f;
    const PrototypeId door = f.RegisterDoor();
    REQUIRE(f.mSchedule.Build().has_value());
    WindowSpawn spawn{.mFixture = &f, .mPrototype = door, .mDespawn = EntityId::Invalid(), .mPending = {}};
    REQUIRE(f.mSchedule.RunWindow(&SpawnInWindow, &spawn).has_value());
    const std::vector<EntityId> objects = f.Objects();
    const EntityId root                 = f.RootOf(objects);
    const EntityId panel                = objects[0] == root ? objects[1] : objects[0];

    WindowSpawn despawnPanel{.mFixture = &f, .mPrototype = PrototypeId::Invalid(), .mDespawn = panel, .mPending = {}};
    REQUIRE(f.mSchedule.RunWindow(&SpawnInWindow, &despawnPanel).has_value());
    CHECK(f.mLayer.GetLastHookStats().mDespawning == 1);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value());
    CHECK(f.mWorld.IsAlive(root));
    CHECK_FALSE(f.mWorld.IsAlive(panel));
    CHECK(gLog.mDespawning == 0); // the brain belongs to the root

    // A brain that despawns its owner in OnSpawned: Spawned first, Despawning at the next sync point.
    gLog.mDespawnOnSpawn = true;
    f.mSeen.mSpawned.clear();
    f.mSeen.mDespawning.clear();
    WindowSpawn spawnDoomed{.mFixture = &f, .mPrototype = door, .mDespawn = EntityId::Invalid(), .mPending = {}};
    REQUIRE(f.mSchedule.RunWindow(&SpawnInWindow, &spawnDoomed).has_value());
    CHECK(f.mLayer.GetLastHookStats().mSpawned == 2);
    CHECK(f.mLayer.GetLastHookStats().mDespawning == 0);
    CHECK(f.Objects().size() == 3);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value()); // Spawned arrives; Despawning goes out
    CHECK(f.mSeen.mSpawned.size() == 2);
    CHECK(f.mLayer.GetLastHookStats().mDespawning == 2);
    CHECK(f.Objects().size() == 3);
    REQUIRE(f.mSchedule.RunPhaseSerial(Phase::Variable).has_value()); // Despawning arrives; the entities die
    CHECK(f.mSeen.mDespawning.size() == 2);
    CHECK(f.Objects().size() == 1);
    CHECK(f.mWorld.IsAlive(root));
}
