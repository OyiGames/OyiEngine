#pragma once

#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/World.h>

#include <cstdint>

namespace oyi::ecs {

/// The firewall window (ADR-0006): a single-threaded stretch right after a sync point where no
/// job is in flight, every command buffer has been replayed and the world stands still. Scripts
/// and OO code run here with functions and entity ids only: reads go to the live data (safe,
/// nothing moves) or to the Prev snapshot; writes are recorded as commands and messages that
/// take effect when the window closes, so no pointer into live data ever leaves the window.
class FirewallWindow {
public:
    FirewallWindow(World& world, const Snapshot* prev, CommandBuffer& commands, MessageBuffer& outbox,
                   void* data) noexcept
        : mWorld(world), mPrev(prev), mCommands(commands), mOutbox(outbox), mData(data) {}
    FirewallWindow(const FirewallWindow&)            = delete;
    FirewallWindow(FirewallWindow&&)                 = delete;
    FirewallWindow& operator=(const FirewallWindow&) = delete;
    FirewallWindow& operator=(FirewallWindow&&)      = delete;
    ~FirewallWindow()                                = default;

    [[nodiscard]] const World& GetWorld() const noexcept { return mWorld; }
    /// nullptr when the schedule captures no snapshot.
    [[nodiscard]] const Snapshot* GetPrev() const noexcept { return mPrev; }
    [[nodiscard]] void* GetData() const noexcept { return mData; }

    template <typename T>
    [[nodiscard]] T* GetData() const noexcept {
        return static_cast<T*>(mData);
    }

    /// World version the window sees; the writes it records land under the next one.
    [[nodiscard]] uint32_t GetVersion() const noexcept { return mWorld.GetVersion(); }

    // --- reads (the world stands still) ---------------------------------------------------------

    [[nodiscard]] bool IsAlive(EntityId entity) const noexcept { return mWorld.IsAlive(entity); }
    [[nodiscard]] bool HasComponent(EntityId entity, ComponentTypeIndex type) const noexcept {
        return mWorld.HasComponent(entity, type);
    }
    [[nodiscard]] bool IsComponentEnabled(EntityId entity, ComponentTypeIndex type) const noexcept {
        return mWorld.IsComponentEnabled(entity, type);
    }
    [[nodiscard]] const ComponentMask* GetComponentMask(EntityId entity) const noexcept {
        return mWorld.GetComponentMask(entity);
    }

    /// The live value; nullptr for a dead entity or a missing type. Valid until the window closes.
    template <typename T>
    [[nodiscard]] const T* Get(EntityId entity, ComponentTypeIndex type) const noexcept {
        return static_cast<const T*>(mWorld.GetComponent(entity, type));
    }

    /// The value at the last sync point; nullptr without a snapshot or when it was not captured.
    template <typename T>
    [[nodiscard]] const T* Prev(EntityId entity, ComponentTypeIndex type) const noexcept {
        return mPrev == nullptr ? nullptr : mPrev->Get<T>(entity, type);
    }

    /// Resources are not chunk memory: with nothing else running, the window may use them
    /// directly. Pointers are for the window's duration only.
    template <typename T>
    [[nodiscard]] const T* GetResource(ResourceTypeIndex type) const noexcept {
        return mWorld.GetResources().Get<T>(type);
    }

    template <typename T>
    [[nodiscard]] T* GetResourceForWrite(ResourceTypeIndex type) noexcept {
        return mWorld.GetResources().Get<T>(type);
    }

    [[nodiscard]] const Environment& GetEnvironment() const noexcept { return mWorld.GetEnvironment(); }
    [[nodiscard]] Environment& GetEnvironmentForWrite() noexcept { return mWorld.GetEnvironmentForWrite(); }

    /// Calls `fn(EntityId)` for every live entity with all of `all` and none of `none`.
    template <typename Fn>
    void ForEachEntity(const ComponentMask& all, const ComponentMask& none, Fn&& fn) {
        mWorld.ForEachChunk(all, none, [&fn](ChunkView& view) {
            for (const EntityId entity : view.GetEntities()) {
                fn(entity);
            }
        });
    }

    // --- writes (take effect when the window closes) --------------------------------------------

    [[nodiscard]] CommandBuffer& GetCommands() noexcept { return mCommands; }

    [[nodiscard]] PendingEntity CreateEntity(const ComponentMask& mask) noexcept {
        return mCommands.CreateEntity(mask);
    }
    void DestroyEntity(EntityId entity) noexcept { mCommands.DestroyEntity(entity); }
    void AddComponent(EntityId entity, ComponentTypeIndex type) noexcept { mCommands.AddComponent(entity, type); }
    void RemoveComponent(EntityId entity, ComponentTypeIndex type) noexcept { mCommands.RemoveComponent(entity, type); }
    void SetComponentEnabled(EntityId entity, ComponentTypeIndex type, bool enabled) noexcept {
        mCommands.SetComponentEnabled(entity, type, enabled);
    }

    template <typename T>
    void Set(EntityId entity, ComponentTypeIndex type, const T& value) noexcept {
        mCommands.Set(entity, type, value);
    }

    template <typename T>
    void Set(PendingEntity entity, ComponentTypeIndex type, const T& value) noexcept {
        mCommands.Set(entity, type, value);
    }

    /// Queues a message for the receivers of `type` (every receiver when `target` is invalid);
    /// routed when the window closes, read on their next run.
    template <typename T>
    void Send(MessageTypeIndex type, EntityId target, const T& value) noexcept {
        mOutbox.Send(type, target, value);
    }

    [[nodiscard]] uint32_t GetPendingCommandCount() const noexcept { return mCommands.GetCommandCount(); }

private:
    World& mWorld;
    const Snapshot* mPrev;
    CommandBuffer& mCommands;
    MessageBuffer& mOutbox;
    void* mData;
};

/// Code that runs inside a firewall window; `window.GetData()` carries its state.
using WindowFunction = void (*)(FirewallWindow& window);

} // namespace oyi::ecs
