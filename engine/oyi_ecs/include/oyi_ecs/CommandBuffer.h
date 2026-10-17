#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/ManagedObject.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

/// An entity created in a CommandBuffer, usable by later commands in the same buffer. It becomes a
/// real EntityId only when the buffer is replayed at a sync point (ADR-0008).
struct PendingEntity {
    uint32_t mIndex = 0xFFFF'FFFFu;

    [[nodiscard]] constexpr bool IsValid() const noexcept { return mIndex != 0xFFFF'FFFFu; }
};

/// Records structural changes and component writes for replay at the next sync point (ADR-0008).
///
/// Nothing recorded here is visible before replay, not even to the recorder: a destroyed entity
/// stays readable, a created one has no EntityId yet. Each job records into its own buffer; the
/// sync point merges every buffer by sort key, so the outcome does not depend on which worker ran
/// which job. Single-threaded by contract.
class CommandBuffer {
public:
    explicit CommandBuffer(core::Allocator& allocator) noexcept;
    CommandBuffer(const CommandBuffer&)            = delete;
    CommandBuffer(CommandBuffer&&)                 = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;
    CommandBuffer& operator=(CommandBuffer&&)      = delete;
    ~CommandBuffer()                               = default;

    /// Replay order across buffers: ascending sort key, then buffer order, then recording order.
    /// Systems use (system id, chunk index) so the result is independent of worker timing.
    void SetSortKey(uint64_t key) noexcept { mSortKey = key; }
    [[nodiscard]] uint64_t GetSortKey() const noexcept { return mSortKey; }

    [[nodiscard]] PendingEntity CreateEntity(const ComponentMask& mask) noexcept;
    /// World::CloneEntity at replay (ADR-0024: a prototype instance). Skipped if `source` died.
    [[nodiscard]] PendingEntity CloneEntity(EntityId source) noexcept;
    void DestroyEntity(EntityId entity) noexcept;
    /// World::SetMark at replay.
    void SetMark(EntityId entity, EntityMark mark) noexcept;
    void SetMark(PendingEntity entity, EntityMark mark) noexcept;

    void AddComponent(EntityId entity, ComponentTypeIndex type) noexcept;
    void AddComponent(PendingEntity entity, ComponentTypeIndex type) noexcept;
    void RemoveComponent(EntityId entity, ComponentTypeIndex type) noexcept;
    void RemoveComponent(PendingEntity entity, ComponentTypeIndex type) noexcept;

    /// Overwrites the whole component with `size` bytes copied now. `size` must equal the
    /// component's size; the replay skips the command otherwise.
    void SetComponent(EntityId entity, ComponentTypeIndex type, const void* data, uint32_t size) noexcept;
    void SetComponent(PendingEntity entity, ComponentTypeIndex type, const void* data, uint32_t size) noexcept;

    template <typename T>
    void Set(EntityId entity, ComponentTypeIndex type, const T& value) noexcept {
        SetComponent(entity, type, &value, static_cast<uint32_t>(sizeof(T)));
    }

    template <typename T>
    void Set(PendingEntity entity, ComponentTypeIndex type, const T& value) noexcept {
        SetComponent(entity, type, &value, static_cast<uint32_t>(sizeof(T)));
    }

    void SetComponentEnabled(EntityId entity, ComponentTypeIndex type, bool enabled) noexcept;
    void SetComponentEnabled(PendingEntity entity, ComponentTypeIndex type, bool enabled) noexcept;

    /// Overwrites the state block of the managed object `id` owned by `owner` with `size` bytes
    /// copied now (ADR-0024). Skipped at replay when the id is stale, the owner differs, the
    /// object is untyped or `size` is not the state size.
    void WriteManagedState(EntityId owner, ManagedId id, const void* data, uint32_t size) noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept { return mCommands.empty(); }
    [[nodiscard]] uint32_t GetCommandCount() const noexcept { return static_cast<uint32_t>(mCommands.size()); }
    [[nodiscard]] uint32_t GetCreateCount() const noexcept { return static_cast<uint32_t>(mCreateMasks.size()); }
    /// Forgets every command and payload; the memory stays reserved for the next frame.
    void Clear() noexcept;

private:
    friend class World;

    enum class Op : uint8_t { Create, Clone, Destroy, Add, Remove, Set, Enable, Mark, ManagedState };

    struct Command {
        uint64_t mSortKey = 0;
        Op mOp            = Op::Create;
        bool mFlag        = false;            ///< Enable: the new state
        EntityMark mMark  = EntityMark::None; ///< Mark: the new mark
        ManagedId mManaged{};                 ///< ManagedState: the object
        ComponentTypeIndex mType;
        EntityId mEntity;       ///< target when mPending is invalid; Clone: the source
        PendingEntity mPending; ///< target created earlier in this buffer; Create / Clone: the slot it fills
        uint32_t mPayloadOffset = 0;
        uint32_t mPayloadSize   = 0;
    };

    void Record(Op op, ComponentTypeIndex type, EntityId entity, PendingEntity pending, uint32_t payloadOffset,
                uint32_t payloadSize, bool flag) noexcept;
    [[nodiscard]] uint32_t StorePayload(const void* data, uint32_t size) noexcept;

    core::MemoryResource mResource;
    std::pmr::vector<Command> mCommands;
    std::pmr::vector<ComponentMask> mCreateMasks;
    std::pmr::vector<std::byte> mPayload;
    uint64_t mSortKey = 0;
};

} // namespace oyi::ecs
