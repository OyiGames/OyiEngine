#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Handle.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>

#include <cstdint>
#include <memory_resource>
#include <type_traits>
#include <utility>
#include <vector>

namespace oyi::ecs {

/// Managed components (ADR-0006): the chunk holds a handle, the object lives on the heap and has
/// a virtual Update that runs on the main thread. The handle is what ADR-0006 calls "the pointer
/// in the chunk", with a generation so a stale reference resolves to nothing instead of garbage.
struct ManagedTag {};
using ManagedId = core::Handle<ManagedTag>;

class ManagedContext;

/// Base class of every managed object. Owned by a ManagedPool, bound to one entity.
class ManagedObject {
public:
    ManagedObject()                                = default;
    ManagedObject(const ManagedObject&)            = delete;
    ManagedObject(ManagedObject&&)                 = delete;
    ManagedObject& operator=(const ManagedObject&) = delete;
    ManagedObject& operator=(ManagedObject&&)      = delete;
    virtual ~ManagedObject()                       = default;

    virtual void Update(ManagedContext& context) = 0;

    [[nodiscard]] EntityId GetOwner() const noexcept { return mOwner; }
    [[nodiscard]] ManagedId GetId() const noexcept { return mId; }

private:
    friend class ManagedPool;

    EntityId mOwner;
    ManagedId mId;
};

/// Owns the managed objects of one type for one World. Objects are created through the pool,
/// addressed by ManagedId, and destroyed either explicitly or when their owner entity is gone.
class ManagedPool {
public:
    explicit ManagedPool(core::Allocator& allocator) noexcept;
    ManagedPool(const ManagedPool&)            = delete;
    ManagedPool(ManagedPool&&)                 = delete;
    ManagedPool& operator=(const ManagedPool&) = delete;
    ManagedPool& operator=(ManagedPool&&)      = delete;
    /// Destroys every remaining object.
    ~ManagedPool();

    /// Constructs a T owned by `owner`. Fails with OutOfMemory.
    template <typename T, typename... Args>
    [[nodiscard]] core::Expected<ManagedId> Create(EntityId owner, Args&&... args) noexcept {
        static_assert(std::is_base_of_v<ManagedObject, T>, "managed objects derive from ManagedObject");
        T* const object = mAllocator.New<T>(std::forward<Args>(args)...);
        if (object == nullptr) {
            return core::Fail(core::ErrorCode::OutOfMemory, "ManagedPool: cannot allocate the object");
        }
        return Adopt(object, owner);
    }

    /// nullptr for a stale or invalid id.
    [[nodiscard]] ManagedObject* Resolve(ManagedId id) noexcept;
    [[nodiscard]] const ManagedObject* Resolve(ManagedId id) const noexcept;

    template <typename T>
    [[nodiscard]] T* Resolve(ManagedId id) noexcept {
        return static_cast<T*>(Resolve(id));
    }

    [[nodiscard]] bool IsAlive(ManagedId id) const noexcept { return Resolve(id) != nullptr; }
    /// Deletes the object; later Resolve calls with the id return nullptr.
    void Destroy(ManagedId id) noexcept;
    /// Deletes every object whose owner is no longer alive in `world`; returns how many.
    uint32_t CollectDead(const World& world) noexcept;

    [[nodiscard]] uint32_t GetLiveCount() const noexcept { return mLiveCount; }

    /// Calls `fn(ManagedObject&)` for every live object in slot order.
    template <typename Fn>
    void ForEach(Fn&& fn) {
        for (const Slot& slot : mSlots) {
            if (slot.mObject != nullptr) {
                fn(*slot.mObject);
            }
        }
    }

private:
    struct Slot {
        ManagedObject* mObject = nullptr;
        uint32_t mGeneration   = 1;
    };

    [[nodiscard]] core::Expected<ManagedId> Adopt(ManagedObject* object, EntityId owner) noexcept;
    void Release(uint32_t index) noexcept;

    core::Allocator& mAllocator;
    core::MemoryResource mResource;
    std::pmr::vector<Slot> mSlots;
    std::pmr::vector<uint32_t> mFreeIndices;
    uint32_t mLiveCount = 0;
};

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

/// The chunk-side half of a managed component: a handle into a ManagedPool. Declare one
/// OYI_COMPONENT per managed type that embeds a ManagedId so the type can be queried.
struct ManagedReference {
    ManagedId mObject;
};

} // namespace oyi::ecs
