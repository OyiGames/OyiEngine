#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/Handle.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_core/Schema.h>
#include <oyi_core/TypeId.h>
#include <oyi_ecs/Entity.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource> // IWYU pragma: keep (libc++ sees no use, the MSVC STL needs it)
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace oyi::ecs {

class FirewallWindow;
class World;

/// Managed components (ADR-0006): the chunk holds a handle, the object lives on the heap and has
/// a virtual Update that runs on the main thread. The handle is what ADR-0006 calls "the pointer
/// in the chunk", with a generation so a stale reference resolves to nothing instead of garbage.
struct ManagedTag {};
using ManagedId = core::Handle<ManagedTag>;

// --- access declaration (ADR-0006: managed components declare their access too) -------------------

/// Component types a managed type reads / writes, message types it receives / sends. Resolved
/// against a World's registries when the managed system is described.
template <typename... Ts>
struct Read {
    static constexpr std::array<const core::ComponentInfo*, sizeof...(Ts)> kInfos{&core::ComponentTraits<Ts>::kInfo...};
};
template <typename... Ts>
struct Write {
    static constexpr std::array<const core::ComponentInfo*, sizeof...(Ts)> kInfos{&core::ComponentTraits<Ts>::kInfo...};
};
template <typename... Ts>
struct Receive {
    static constexpr std::array<const core::ComponentInfo*, sizeof...(Ts)> kInfos{&core::ComponentTraits<Ts>::kInfo...};
};
template <typename... Ts>
struct Send {
    static constexpr std::array<const core::ComponentInfo*, sizeof...(Ts)> kInfos{&core::ComponentTraits<Ts>::kInfo...};
};

template <typename R = Read<>, typename W = Write<>, typename Rc = Receive<>, typename S = Send<>>
struct Access {
    using Reads    = R;
    using Writes   = W;
    using Receives = Rc;
    using Sends    = S;
};

/// Everything the engine knows about a managed type (ADR-0024): identity and field table come
/// from the state block, an OYI_STRUCT that holds every member tools may see or save; the access
/// sets gate what Update may touch; mCreate rebuilds an object from a world file.
struct ManagedTypeInfo {
    const core::ComponentInfo* mState = nullptr; ///< the state block's reflection; its TypeId is the managed type's
    std::span<const core::ComponentInfo* const> mReads;
    std::span<const core::ComponentInfo* const> mWrites;
    std::span<const core::ComponentInfo* const> mReceives;
    std::span<const core::ComponentInfo* const> mSends;
    class ManagedObject* (*mCreate)(core::Allocator& allocator) noexcept = nullptr;

    [[nodiscard]] core::TypeId GetTypeId() const noexcept { return mState->mTypeId; }
    [[nodiscard]] std::string_view GetName() const noexcept { return mState->mName; }
    [[nodiscard]] uint32_t GetStateSize() const noexcept { return mState->mSize; }
};

/// Base class of every managed object. Owned by a ManagedPool, bound to one entity. Types that
/// use OYI_MANAGED are reflected, saved and scriptable; others are run-time only.
class ManagedObject {
public:
    ManagedObject()                                = default;
    ManagedObject(const ManagedObject&)            = delete;
    ManagedObject(ManagedObject&&)                 = delete;
    ManagedObject& operator=(const ManagedObject&) = delete;
    ManagedObject& operator=(ManagedObject&&)      = delete;
    virtual ~ManagedObject()                       = default;

    virtual void Update(class ManagedContext& context) = 0;
    /// ADR-0024 lifecycle, called by the object layer inside the sync hook (main thread, world
    /// still): after the owner and every entity spawned at the same sync point exist, and before
    /// the owner is destroyed (it is alive for one more sync point).
    virtual void OnSpawned(FirewallWindow& /*window*/) {}
    virtual void OnDespawning(FirewallWindow& /*window*/) {}
    /// After the object was rebuilt from a world file and its state block restored: fill in
    /// run-time members. The world is complete and static.
    virtual void OnLoaded(World& /*world*/) {}

    /// nullptr for a type without OYI_MANAGED.
    [[nodiscard]] virtual const ManagedTypeInfo* GetTypeInfo() const noexcept { return nullptr; }
    /// The state block bytes (GetTypeInfo()->GetStateSize() of them), or nullptr when untyped.
    [[nodiscard]] virtual const void* GetStateBytes() const noexcept { return nullptr; }
    [[nodiscard]] virtual void* GetStateBytesForWrite() noexcept { return nullptr; }

    [[nodiscard]] EntityId GetOwner() const noexcept { return mOwner; }
    [[nodiscard]] ManagedId GetId() const noexcept { return mId; }

private:
    friend class ManagedPool;

    EntityId mOwner;
    ManagedId mId;
};

namespace detail {

template <typename Derived, typename State, typename AccessSpec>
[[nodiscard]] ManagedTypeInfo MakeManagedTypeInfo() noexcept {
    static_assert(std::is_base_of_v<ManagedObject, Derived>, "OYI_MANAGED types derive from ManagedObject");
    static_assert(std::is_default_constructible_v<Derived>,
                  "OYI_MANAGED types need a default constructor: world files rebuild them with it");
    return ManagedTypeInfo{
        .mState    = &core::ComponentTraits<State>::kInfo,
        .mReads    = AccessSpec::Reads::kInfos,
        .mWrites   = AccessSpec::Writes::kInfos,
        .mReceives = AccessSpec::Receives::kInfos,
        .mSends    = AccessSpec::Sends::kInfos,
        .mCreate   = [](core::Allocator& allocator) noexcept -> ManagedObject* { return allocator.New<Derived>(); },
    };
}

} // namespace detail

/// Inside a ManagedObject subclass: declares its state block (an OYI_STRUCT member `mState`), its
/// access sets and the type information (ADR-0024). Example:
///
///   class DoorBrain final : public ManagedObject {
///       OYI_MANAGED(DoorBrain, DoorBrainState, Access<Read<DoorState>, Write<DoorState>>)
///       void Update(ManagedContext& context) override { ... mState.mCooldown ... }
///   };
///
/// The block is private to the class (GetState / GetStateForWrite outside); members other than
/// mState are run-time private: never saved, never shown; OnLoaded rebuilds them.
#define OYI_MANAGED(Class, State, ...)                                                                                 \
public:                                                                                                                \
    using ManagedState  = State;                                                                                       \
    using ManagedAccess = __VA_ARGS__;                                                                                 \
    [[nodiscard]] static const ::oyi::ecs::ManagedTypeInfo& GetManagedTypeInfo() noexcept {                            \
        static const ::oyi::ecs::ManagedTypeInfo kInfo =                                                               \
            ::oyi::ecs::detail::MakeManagedTypeInfo<Class, State, __VA_ARGS__>();                                      \
        return kInfo;                                                                                                  \
    }                                                                                                                  \
    [[nodiscard]] const ::oyi::ecs::ManagedTypeInfo* GetTypeInfo() const noexcept override {                           \
        return &GetManagedTypeInfo();                                                                                  \
    }                                                                                                                  \
    [[nodiscard]] const void* GetStateBytes() const noexcept override {                                                \
        return &mState;                                                                                                \
    }                                                                                                                  \
    [[nodiscard]] void* GetStateBytesForWrite() noexcept override {                                                    \
        return &mState;                                                                                                \
    }                                                                                                                  \
    [[nodiscard]] const State& GetState() const noexcept {                                                             \
        return mState;                                                                                                 \
    }                                                                                                                  \
    [[nodiscard]] State& GetStateForWrite() noexcept {                                                                 \
        return mState;                                                                                                 \
    }                                                                                                                  \
                                                                                                                       \
private:                                                                                                               \
    State mState{};                                                                                                    \
                                                                                                                       \
private:

/// Owns the managed objects of one World. Objects are created through the pool, addressed by
/// ManagedId, and destroyed either explicitly or when their owner entity is gone.
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

    /// Takes ownership of an object allocated with GetAllocator() (a type factory's product) and
    /// binds it to `owner`.
    [[nodiscard]] core::Expected<ManagedId> Adopt(ManagedObject* object, EntityId owner) noexcept;
    [[nodiscard]] core::Allocator& GetAllocator() const noexcept { return mAllocator; }

    /// Places an object rebuilt from a world file at exactly `id` (World::Load). The slot must be
    /// free; the generation is taken from the id so handles in chunks keep resolving.
    [[nodiscard]] core::Result Restore(ManagedId id, ManagedObject* object, EntityId owner) noexcept;

    /// nullptr for a stale or invalid id.
    [[nodiscard]] ManagedObject* Resolve(ManagedId id) noexcept;
    [[nodiscard]] const ManagedObject* Resolve(ManagedId id) const noexcept;

    template <typename T>
    [[nodiscard]] T* Resolve(ManagedId id) noexcept {
        return static_cast<T*>(Resolve(id));
    }

    [[nodiscard]] bool IsAlive(ManagedId id) const noexcept { return Resolve(id) != nullptr; }
    /// The live object of managed type `type` (a state block TypeId) owned by `owner`, or nullptr.
    [[nodiscard]] ManagedObject* FindByOwner(EntityId owner, core::TypeId type) noexcept;
    [[nodiscard]] const ManagedObject* FindByOwner(EntityId owner, core::TypeId type) const noexcept;

    /// Deletes the object; later Resolve calls with the id return nullptr.
    void Destroy(ManagedId id) noexcept;
    /// Deletes every object whose owner is no longer alive in `world`; returns how many.
    uint32_t CollectDead(const World& world) noexcept;
    /// Deletes every object; slots and generations start over.
    void Clear() noexcept;
    [[nodiscard]] uint32_t GetLiveCount() const noexcept { return mLiveCount; }
    [[nodiscard]] uint32_t GetSlotCount() const noexcept { return static_cast<uint32_t>(mSlots.size()); }

    /// Calls `fn(ManagedObject&)` for every live object in slot order.
    template <typename Fn>
    void ForEach(Fn&& fn) {
        for (const Slot& slot : mSlots) {
            if (slot.mObject != nullptr) {
                fn(*slot.mObject);
            }
        }
    }

    template <typename Fn>
    void ForEach(Fn&& fn) const {
        for (const Slot& slot : mSlots) {
            if (slot.mObject != nullptr) {
                fn(static_cast<const ManagedObject&>(*slot.mObject));
            }
        }
    }

private:
    struct Slot {
        ManagedObject* mObject = nullptr;
        uint32_t mGeneration   = 1;
    };

    void Release(uint32_t index) noexcept;

    core::Allocator& mAllocator;
    core::MemoryResource mResource;
    std::pmr::vector<Slot> mSlots;
    std::pmr::vector<uint32_t> mFreeIndices;
    uint32_t mLiveCount = 0;
};

/// The managed types a World knows (ADR-0024): what Load can rebuild and what scripts and tools
/// can name. Register every OYI_MANAGED type before loading a world that contains it.
class ManagedTypeRegistry {
public:
    explicit ManagedTypeRegistry(core::Allocator& allocator) noexcept;
    ManagedTypeRegistry(const ManagedTypeRegistry&)            = delete;
    ManagedTypeRegistry(ManagedTypeRegistry&&)                 = delete;
    ManagedTypeRegistry& operator=(const ManagedTypeRegistry&) = delete;
    ManagedTypeRegistry& operator=(ManagedTypeRegistry&&)      = delete;
    ~ManagedTypeRegistry()                                     = default;

    /// Idempotent per type. Fails with InvalidArgument for a type without a state block.
    [[nodiscard]] core::Result Register(const ManagedTypeInfo& info) noexcept;

    template <typename T>
    [[nodiscard]] core::Result Register() noexcept {
        return Register(T::GetManagedTypeInfo());
    }

    [[nodiscard]] const ManagedTypeInfo* Find(core::TypeId type) const noexcept;
    [[nodiscard]] const ManagedTypeInfo* Find(std::string_view name) const noexcept;
    [[nodiscard]] std::span<const ManagedTypeInfo* const> GetAll() const noexcept { return mTypes; }
    [[nodiscard]] uint32_t GetCount() const noexcept { return static_cast<uint32_t>(mTypes.size()); }

private:
    core::MemoryResource mResource;
    std::pmr::vector<const ManagedTypeInfo*> mTypes;
};

} // namespace oyi::ecs
