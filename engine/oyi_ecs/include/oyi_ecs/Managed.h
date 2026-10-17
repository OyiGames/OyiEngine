#pragma once

#include <oyi_core/Expected.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/ManagedObject.h> // IWYU pragma: export
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>

#include <cstdint>

namespace oyi::ecs {

/// What a managed object sees while it updates: its owner, the Prev snapshot (ADR-0006: the
/// managed layer reads the previous sync point) and the system context with its declared access.
class ManagedContext {
public:
    ManagedContext(SystemContext& system, EntityId owner, const Snapshot* prev) noexcept
        : mSystem(system), mOwner(owner), mPrev(prev) {}

    [[nodiscard]] SystemContext& GetSystem() noexcept { return mSystem; }
    [[nodiscard]] EntityId GetOwner() const noexcept { return mOwner; }
    /// nullptr when the managed system was given no snapshot.
    [[nodiscard]] const Snapshot* GetPrev() const noexcept { return mPrev; }
    [[nodiscard]] CommandBuffer& GetCommands() noexcept { return mSystem.GetCommands(); }

    /// The owner's previous-sync-point value, or nullptr (no snapshot, not captured, no type).
    template <typename T>
    [[nodiscard]] const T* Prev(ComponentTypeIndex type) const noexcept {
        return mPrev == nullptr ? nullptr : mPrev->Get<T>(mOwner, type);
    }

    template <typename T>
    [[nodiscard]] const T* Lookup(ComponentTypeIndex type, EntityId entity) const noexcept {
        return mSystem.Lookup<T>(type, entity);
    }

    template <typename T>
    [[nodiscard]] T* LookupForWrite(ComponentTypeIndex type, EntityId entity) noexcept {
        return mSystem.LookupForWrite<T>(type, entity);
    }

    template <typename T>
    void Send(MessageTypeIndex type, EntityId target, const T& value) noexcept {
        mSystem.Send(type, target, value);
    }

    /// Calls `fn(const T&)` for every message of `type` addressed to the owner or broadcast
    /// (ADR-0024: a managed object receives through its declared Receive set).
    template <typename T, typename Fn>
    void ForEachReceived(MessageTypeIndex type, Fn&& fn) const {
        const ReceivedMessages<T> received = mSystem.Received<T>(type);
        for (size_t i = 0; i < received.mPayloads.size(); ++i) {
            if (!received.mTargets[i].IsValid() || received.mTargets[i] == mOwner) {
                fn(received.mPayloads[i]);
            }
        }
    }

    template <typename T>
    [[nodiscard]] const T* GetResource(ResourceTypeIndex type) const noexcept {
        return mSystem.GetResource<T>(type);
    }

    template <typename T>
    [[nodiscard]] T* GetResourceForWrite(ResourceTypeIndex type) noexcept {
        return mSystem.GetResourceForWrite<T>(type);
    }

    [[nodiscard]] const Environment& GetEnvironment() const noexcept { return mSystem.GetEnvironment(); }

private:
    SystemContext& mSystem;
    EntityId mOwner;
    const Snapshot* mPrev;
};

/// Turns one managed type into a main-thread system (ADR-0006: the managed system group). The
/// reference component holds the ManagedId of the owner's object. On every run the system first
/// collects the objects of dead owners, then updates the live ones in chunk order.
class ManagedSystem {
public:
    ManagedSystem(ManagedPool& pool, ComponentTypeIndex referenceType, const Snapshot* prev) noexcept
        : mPool(pool), mReferenceType(referenceType), mPrev(prev) {}

    /// Completes `desc` for Schedule::AddSystem: adds the reference component to the read set,
    /// marks the system as main-thread and binds it to this object. The declared read / write /
    /// send / receive masks of `desc` are what every Update of this type may touch.
    [[nodiscard]] SystemDesc Describe(SystemDesc desc) const noexcept;
    /// The same, with the access sets of an OYI_MANAGED type added (ApplyManagedAccess).
    [[nodiscard]] core::Expected<SystemDesc> Describe(SystemDesc desc, const ManagedTypeInfo& info,
                                                      const World& world) const noexcept;

    [[nodiscard]] ManagedPool& GetPool() const noexcept { return mPool; }
    [[nodiscard]] uint32_t GetLastUpdateCount() const noexcept { return mLastUpdates; }
    [[nodiscard]] uint32_t GetLastCollected() const noexcept { return mLastCollected; }

private:
    static void Run(SystemContext& context);

    ManagedPool& mPool;
    ComponentTypeIndex mReferenceType;
    const Snapshot* mPrev;
    uint32_t mLastUpdates   = 0;
    uint32_t mLastCollected = 0;
};

/// Adds the access sets of an OYI_MANAGED type to `desc` (ADR-0024): reads, writes, receives and
/// sends resolved against `world`'s registries. Fails with NotFound when a listed type is not
/// registered in the world; `desc` is untouched then.
[[nodiscard]] core::Result ApplyManagedAccess(const ManagedTypeInfo& info, const World& world,
                                              SystemDesc& desc) noexcept;

/// The chunk-side half of a managed component: a handle into a ManagedPool. Declare one
/// OYI_COMPONENT per managed type that embeds a ManagedId so the type can be queried.
struct ManagedReference {
    ManagedId mObject;
};

} // namespace oyi::ecs
