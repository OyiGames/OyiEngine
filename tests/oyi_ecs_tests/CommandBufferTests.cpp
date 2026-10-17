#include <oyi_core/SystemAllocator.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/ComponentRegistry.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/World.h>
#include <oyi_ecs_fixture/Components.gen.h> // IWYU pragma: keep
#include <oyi_ecs_fixture/Components.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobBuilder.h>
#include <oyi_job/JobSystem.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <span>
#include <vector>

namespace {

using oyi::core::SystemAllocator;
using oyi::ecs::CommandBuffer;
using oyi::ecs::ComponentMask;
using oyi::ecs::ComponentTypeIndex;
using oyi::ecs::EntityId;
using oyi::ecs::EntityMark;
using oyi::ecs::PendingEntity;
using oyi::ecs::World;
using namespace oyi::ecs_fixture;

struct Fixture {
    SystemAllocator mAllocator;
    World mWorld{mAllocator};
    ComponentTypeIndex mPosition = *mWorld.GetComponents().Register<Position>();
    ComponentTypeIndex mVelocity = *mWorld.GetComponents().Register<Velocity>();
    ComponentTypeIndex mHealth   = *mWorld.GetComponents().Register<Health>();
    ComponentTypeIndex mFrozen   = *mWorld.GetComponents().Register<Frozen>();
};

[[nodiscard]] ComponentMask MaskOf(std::initializer_list<ComponentTypeIndex> types) {
    ComponentMask mask;
    for (const ComponentTypeIndex type : types) {
        mask.Set(type);
    }
    return mask;
}

/// What a Call command's callback saw.
struct CallLog {
    std::vector<EntityId> mTargets;
    std::vector<uint8_t> mBytes;
    uint32_t mAlive = 0;
};

// The signature is CommandCallback's; the world is only read.
void OnCall(World& world, EntityId target, void* data,
            std::span<const std::byte> payload) { // NOLINT(misc-const-correctness)
    auto* const log = static_cast<CallLog*>(data);
    log->mTargets.push_back(target);
    log->mAlive += world.IsAlive(target) ? 1 : 0;
    for (const std::byte byte : payload) {
        log->mBytes.push_back(static_cast<uint8_t>(byte));
    }
}

} // namespace

TEST_CASE("nothing recorded in a command buffer is visible before replay", "[ecs][commands]") {
    Fixture f;
    const EntityId victim = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    CommandBuffer commands(f.mAllocator);

    const PendingEntity pending = commands.CreateEntity(MaskOf({f.mPosition, f.mHealth}));
    CHECK(pending.IsValid());
    commands.Set(pending, f.mPosition, Position{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f});
    commands.DestroyEntity(victim);
    commands.AddComponent(victim, f.mVelocity); // never applies: the entity is gone by then
    CHECK(commands.GetCommandCount() == 4);
    CHECK(commands.GetCreateCount() == 1);

    // The recorder sees the old world until the sync point.
    CHECK(f.mWorld.GetEntityCount() == 1);
    CHECK(f.mWorld.IsAlive(victim));

    const std::array<CommandBuffer*, 1> buffers{&commands};
    const World::ReplayStats stats = f.mWorld.SyncPoint(std::span<CommandBuffer* const>{buffers});
    CHECK(stats.mApplied == 3);
    CHECK(stats.mSkipped == 1);
    CHECK(commands.IsEmpty());

    CHECK_FALSE(f.mWorld.IsAlive(victim));
    CHECK(f.mWorld.GetEntityCount() == 1);
    uint32_t seen = 0;
    f.mWorld.ForEachChunk(MaskOf({f.mPosition, f.mHealth}), ComponentMask{}, [&](oyi::ecs::ChunkView& view) {
        for (const Position& position : view.Read<Position>(f.mPosition)) {
            CHECK(position.mX == 1.0f);
            CHECK(position.mZ == 3.0f);
            ++seen;
        }
        for (const Health& health : view.Read<Health>(f.mHealth)) {
            CHECK(health.mCurrent == 100); // defaulted, no Set recorded
        }
    });
    CHECK(seen == 1);
}

TEST_CASE("every command kind replays and the ones aimed at dead entities are skipped", "[ecs][commands]") {
    Fixture f;
    const EntityId entity = *f.mWorld.CreateEntity(MaskOf({f.mPosition, f.mFrozen}));
    const EntityId dead   = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    REQUIRE(f.mWorld.DestroyEntity(dead).has_value());

    CommandBuffer commands(f.mAllocator);
    commands.AddComponent(entity, f.mHealth);
    commands.Set(entity, f.mHealth, Health{.mCurrent = 7, .mMax = 9, .mLastAttacker = dead});
    commands.RemoveComponent(entity, f.mFrozen);
    commands.SetComponentEnabled(entity, f.mPosition, false);
    commands.AddComponent(entity, f.mHealth);                            // duplicate add: skipped
    commands.RemoveComponent(entity, f.mVelocity);                       // never had it: skipped
    commands.SetComponent(entity, f.mPosition, &entity, sizeof(entity)); // wrong size: skipped
    commands.Set(dead, f.mPosition, Position{});                         // dead: skipped
    commands.SetComponentEnabled(dead, f.mPosition, true);

    const std::array<CommandBuffer*, 1> buffers{&commands};
    const World::ReplayStats stats = f.mWorld.ReplayCommands(std::span<CommandBuffer* const>{buffers});
    CHECK(stats.mApplied == 4);
    CHECK(stats.mSkipped == 5);

    CHECK(f.mWorld.HasComponent(entity, f.mHealth));
    CHECK(f.mWorld.Get<Health>(entity)->mCurrent == 7);
    CHECK(f.mWorld.Get<Health>(entity)->mLastAttacker == dead);
    CHECK_FALSE(f.mWorld.HasComponent(entity, f.mFrozen));
    CHECK_FALSE(f.mWorld.IsComponentEnabled(entity, f.mPosition));
}

TEST_CASE("replay order follows the sort key, then buffer order, then recording order", "[ecs][commands]") {
    Fixture f;
    const EntityId entity = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    CommandBuffer late(f.mAllocator);
    CommandBuffer early(f.mAllocator);
    late.SetSortKey(2);
    late.Set(entity, f.mPosition, Position{.mX = 20.0f, .mY = 0.0f, .mZ = 0.0f});
    late.Set(entity, f.mPosition, Position{.mX = 21.0f, .mY = 0.0f, .mZ = 0.0f});
    early.SetSortKey(1);
    early.Set(entity, f.mPosition, Position{.mX = 10.0f, .mY = 0.0f, .mZ = 0.0f});

    // Buffer order in the span does not matter when the keys differ.
    const std::array<CommandBuffer*, 2> buffers{&late, &early};
    (void)f.mWorld.ReplayCommands(std::span<CommandBuffer* const>{buffers});
    CHECK(f.mWorld.Get<Position>(entity)->mX == 21.0f);

    // Equal keys: the span order decides, and recording order within a buffer is kept.
    late.Set(entity, f.mPosition, Position{.mX = 5.0f, .mY = 0.0f, .mZ = 0.0f});
    early.SetSortKey(2);
    early.Set(entity, f.mPosition, Position{.mX = 6.0f, .mY = 0.0f, .mZ = 0.0f});
    early.Set(entity, f.mPosition, Position{.mX = 7.0f, .mY = 0.0f, .mZ = 0.0f});
    (void)f.mWorld.ReplayCommands(std::span<CommandBuffer* const>{buffers});
    CHECK(f.mWorld.Get<Position>(entity)->mX == 7.0f);
}

TEST_CASE("a pending entity can be referenced before it exists and is skipped if its creation failed",
          "[ecs][commands]") {
    Fixture f;
    CommandBuffer commands(f.mAllocator);
    const PendingEntity a = commands.CreateEntity(MaskOf({f.mPosition}));
    const PendingEntity b = commands.CreateEntity(ComponentMask{});
    commands.AddComponent(b, f.mHealth);
    commands.Set(b, f.mHealth, Health{.mCurrent = 1, .mMax = 2, .mLastAttacker = EntityId::Invalid()});
    commands.SetComponentEnabled(a, f.mPosition, false);
    ComponentMask bogus;
    bogus.Set(ComponentTypeIndex{.mValue = 99});
    const PendingEntity broken = commands.CreateEntity(bogus); // unregistered type: creation fails
    commands.AddComponent(broken, f.mPosition);

    const std::array<CommandBuffer*, 1> buffers{&commands};
    const World::ReplayStats stats = f.mWorld.ReplayCommands(std::span<CommandBuffer* const>{buffers});
    CHECK(stats.mApplied == 5);
    CHECK(stats.mSkipped == 2);
    CHECK(f.mWorld.GetEntityCount() == 2);

    uint32_t healthy = 0;
    f.mWorld.ForEachChunk(MaskOf({f.mHealth}), ComponentMask{}, [&](oyi::ecs::ChunkView& view) {
        for (const Health& health : view.Read<Health>(f.mHealth)) {
            CHECK(health.mCurrent == 1);
            ++healthy;
        }
    });
    CHECK(healthy == 1);
    f.mWorld.ForEachChunk(MaskOf({f.mPosition}), ComponentMask{}, [&](oyi::ecs::ChunkView& view) {
        REQUIRE(view.GetCount() == 1);
        CHECK_FALSE(view.IsEnabled(0, f.mPosition));
    });
}

TEST_CASE("commands recorded by parallel jobs replay to the same result every time", "[ecs][commands][job]") {
    Fixture f;
    constexpr uint32_t kEntities = 64;
    constexpr uint32_t kJobs     = 8;
    std::vector<EntityId> entities;
    entities.reserve(kEntities);
    for (uint32_t i = 0; i < kEntities; ++i) {
        entities.push_back(*f.mWorld.CreateEntity(MaskOf({f.mPosition})));
    }

    struct Recorder {
        std::vector<EntityId>* mEntities = nullptr;
        CommandBuffer* mBuffer           = nullptr;
        ComponentTypeIndex mPosition;
        uint32_t mJob = 0;

        static void Run(oyi::job::JobContext& /*context*/, void* data) {
            const auto* const self = static_cast<const Recorder*>(data);
            self->mBuffer->SetSortKey(self->mJob);
            for (uint32_t i = 0; i < self->mEntities->size(); ++i) {
                // Every job writes every entity; only the highest sort key may win.
                self->mBuffer->Set(
                    (*self->mEntities)[i], self->mPosition,
                    Position{.mX = static_cast<float>(self->mJob), .mY = static_cast<float>(i), .mZ = 0.0f});
            }
        }
    };

    const auto runOnce = [&] {
        oyi::job::JobSystem jobs(
            f.mAllocator,
            oyi::job::JobSystem::Config{.mWorkerCount = 3, .mMaxJobs = 64, .mMaxCounters = 16, .mSpinIterations = 8});
        std::vector<CommandBuffer*> buffers;
        buffers.reserve(kJobs);
        std::array<Recorder, kJobs> recorders;
        std::deque<CommandBuffer> storage; // CommandBuffer is not movable; deque never relocates
        for (uint32_t j = 0; j < kJobs; ++j) {
            storage.emplace_back(f.mAllocator);
        }
        const oyi::job::CounterId done = jobs.CreateCounter();
        {
            oyi::job::JobBuilder builder(jobs, "record", oyi::job::JobPriority::Critical,
                                         oyi::job::CounterId::Invalid(), done, oyi::job::JobBuilder::Fence::None);
            for (uint32_t j = 0; j < kJobs; ++j) {
                recorders[j] =
                    Recorder{.mEntities = &entities, .mBuffer = &storage[j], .mPosition = f.mPosition, .mJob = j};
                REQUIRE(builder.DispatchJob("record", &Recorder::Run, &recorders[j]).has_value());
            }
        }
        jobs.WaitForCounter(done);
        jobs.ReleaseCounter(done);
        // Buffers are handed over in an order unrelated to the jobs' completion order.
        for (uint32_t j = 0; j < kJobs; ++j) {
            buffers.push_back(&storage[(j * 5) % kJobs]);
        }
        const World::ReplayStats stats = f.mWorld.SyncPoint(std::span<CommandBuffer* const>{buffers});
        CHECK(stats.mApplied == kEntities * kJobs);
        for (uint32_t i = 0; i < kEntities; ++i) {
            const auto* const position = f.mWorld.Get<Position>(entities[i]);
            REQUIRE(position != nullptr);
            CHECK(position->mX == static_cast<float>(kJobs - 1));
            CHECK(position->mY == static_cast<float>(i));
        }
    };
    for (int round = 0; round < 5; ++round) {
        runOnce();
    }
}

TEST_CASE("clones and marks replay like any other command", "[ecs][commands][object]") {
    Fixture f;
    const EntityId prototype                          = *f.mWorld.CreatePrototype(MaskOf({f.mPosition, f.mHealth}));
    f.mWorld.GetForWrite<Health>(prototype)->mCurrent = 42;
    const EntityId plain                              = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    const EntityId dead                               = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    REQUIRE(f.mWorld.DestroyEntity(dead).has_value());

    CommandBuffer commands(f.mAllocator);
    const PendingEntity instance = commands.CloneEntity(prototype);
    commands.Set(instance, f.mPosition, Position{.mX = 1.0f, .mY = 2.0f, .mZ = 3.0f}); // overrides apply after cloning
    commands.AddComponent(instance, f.mVelocity);                                      // locked: skipped
    commands.SetMark(plain, EntityMark::Locked);
    commands.AddComponent(plain, f.mVelocity);              // locked by then: skipped
    const PendingEntity ghost = commands.CloneEntity(dead); // source is dead: skipped
    commands.Set(ghost, f.mPosition, Position{});           // and so is this
    CHECK(commands.GetCreateCount() == 2);

    const std::array<CommandBuffer*, 1> buffers{&commands};
    const World::ReplayStats stats = f.mWorld.ReplayCommands(std::span<CommandBuffer* const>{buffers});
    CHECK(stats.mApplied == 3);
    CHECK(stats.mSkipped == 4);

    uint32_t locked = 0;
    f.mWorld.ForEachChunk(MaskOf({f.mHealth}), ComponentMask{}, [&](oyi::ecs::ChunkView& view) {
        for (const EntityId entity : view.GetEntities()) {
            locked += f.mWorld.GetMark(entity) == EntityMark::Locked ? 1 : 0;
            CHECK(f.mWorld.Get<Health>(entity)->mCurrent == 42);
            CHECK(f.mWorld.Get<Position>(entity)->mY == 2.0f);
            CHECK_FALSE(f.mWorld.HasComponent(entity, f.mVelocity));
        }
    });
    CHECK(locked == 1);
    CHECK(f.mWorld.GetMark(plain) == EntityMark::Locked);
    CHECK_FALSE(f.mWorld.HasComponent(plain, f.mVelocity));
    CHECK(f.mWorld.GetEntityCount() == 3); // prototype, plain, instance
}

TEST_CASE("link and call commands resolve pending entities at replay", "[ecs][commands][object]") {
    Fixture f;
    const EntityId dead = *f.mWorld.CreateEntity(MaskOf({f.mPosition}));
    REQUIRE(f.mWorld.DestroyEntity(dead).has_value());
    CommandBuffer commands(f.mAllocator);
    CallLog log;
    const PendingEntity owner = commands.CreateEntity(MaskOf({f.mPosition}));
    const PendingEntity child = commands.CreateEntity(MaskOf({f.mPosition, f.mHealth}));
    commands.LinkEntity(child, f.mHealth, static_cast<uint32_t>(offsetof(Health, mLastAttacker)), owner);
    commands.LinkEntity(child, f.mHealth, sizeof(Health) - 2, owner); // does not fit: skipped
    commands.LinkEntity(owner, f.mHealth, 0, child);                  // owner has no Health: skipped
    const std::array<uint8_t, 3> note{7, 8, 9};
    commands.Call(child, &OnCall, &log, note.data(), static_cast<uint32_t>(note.size()));
    commands.Call(owner, &OnCall, &log, nullptr, 0);
    commands.Call(dead, &OnCall, &log, note.data(), 1); // dead target: skipped
    commands.DestroyEntity(dead);                       // dead already: skipped

    const std::array<CommandBuffer*, 1> buffers{&commands};
    const World::ReplayStats stats = f.mWorld.ReplayCommands(std::span<CommandBuffer* const>{buffers});
    CHECK(stats.mApplied == 5);
    CHECK(stats.mSkipped == 4);
    REQUIRE(log.mTargets.size() == 2);
    CHECK(log.mAlive == 2);
    CHECK(f.mWorld.HasComponent(log.mTargets[0], f.mHealth));
    CHECK(f.mWorld.Get<Health>(log.mTargets[0])->mLastAttacker == log.mTargets[1]);
    CHECK(f.mWorld.Get<Health>(log.mTargets[0])->mCurrent == 100); // the bad link touched nothing
    CHECK(log.mBytes == std::vector<uint8_t>{7, 8, 9});
    CHECK(f.mWorld.GetEntityCount() == 2);
}
