#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_ecs/ChunkView.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Entity.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Query.h>
#include <oyi_ecs/Resource.h>
#include <oyi_ecs/World.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace oyi::ecs {

/// The globally ordered update phases (ADR-0007). Phase boundaries are sync points: command
/// buffers replay there and the world version advances.
enum class Phase : uint8_t {
    Begin,
    Input,
    FixedSimulation,
    Variable,
    PhysicsSync,
    TransformPropagate,
    Presentation,
    End,
};

inline constexpr uint32_t kPhaseCount = 8;

[[nodiscard]] constexpr std::string_view ToString(Phase phase) noexcept {
    switch (phase) {
    case Phase::Begin:
        return "Begin";
    case Phase::Input:
        return "Input";
    case Phase::FixedSimulation:
        return "FixedSimulation";
    case Phase::Variable:
        return "Variable";
    case Phase::PhysicsSync:
        return "PhysicsSync";
    case Phase::TransformPropagate:
        return "TransformPropagate";
    case Phase::Presentation:
        return "Presentation";
    case Phase::End:
        return "End";
    }
    return "Unknown";
}

/// Index of a system inside one Schedule.
struct SystemId {
    uint32_t mValue = 0xFFFF'FFFFu;

    [[nodiscard]] static constexpr SystemId Invalid() noexcept { return SystemId{}; }
    [[nodiscard]] constexpr bool IsValid() const noexcept { return mValue != 0xFFFF'FFFFu; }
    [[nodiscard]] constexpr bool operator==(const SystemId&) const noexcept = default;
};

class SystemContext;

/// Systems are stateless functions (ADR-0007); state lives in components, resources or `mData`.
using SystemFunction = void (*)(SystemContext& context);

/// Declares a system: what it reads and writes is its permission set (ADR-0007: the parameter
/// type is the permission). The schedule derives the query (`mRead` and `mWrite` must all be
/// present, `mExclude` must not) and the conflict pairs from these masks.
struct SystemDesc {
    std::string_view mName; ///< mandatory; job name and debug name, must outlive the schedule
    Phase mPhase;
    ComponentMask mRead;
    ComponentMask mWrite;
    ComponentMask mExclude;
    /// Types from mRead / mWrite that an archetype may lack; the query does not require them and
    /// the system checks ChunkView::Has before touching them. They still count as access.
    ComponentMask mOptional;
    MessageMask mSends;    ///< message types this system may send (ADR-0007: MessageSender<M>)
    MessageMask mReceives; ///< message types queued for this system
    /// Resources (ADR-0007: Resource<T>) this system reads and writes; conflict-checked like
    /// components, except that resources registered as Automatic are ordered by the schedule.
    ResourceMask mReadResources;
    ResourceMask mWriteResources;
    /// Runs on the thread that calls RunPhase instead of on a worker (ADR-0006: the managed
    /// system group). Ordered and conflict-checked like any other system; the main thread runs it
    /// as soon as its predecessors are done and helps with jobs while it waits.
    bool mMainThread;
    SystemFunction mFunction;
    void* mData; ///< handed back through SystemContext::GetData
};

/// The messages of one type waiting for a system: targets and payloads side by side.
template <typename T>
struct ReceivedMessages {
    std::span<const EntityId> mTargets; ///< invalid id = broadcast
    std::span<const T> mPayloads;
};

/// What a system sees while it runs. Component access goes through Read / Write so that the
/// declared masks are checked against actual use (ADR-0019, the static half of MAW-lite).
/// Structural changes go through GetCommands and apply at the end of the phase.
class SystemContext {
public:
    SystemContext(World& world, Query& query, CommandBuffer& commands, MessageBuffer& outbox, const MessageInbox& inbox,
                  const SystemDesc& desc, uint32_t version, uint32_t lastVersion, core::Allocator& scratch) noexcept
        : mWorld(world)
        , mQuery(query)
        , mCommands(commands)
        , mOutbox(outbox)
        , mInbox(inbox)
        , mDesc(desc)
        , mVersion(version)
        , mLastVersion(lastVersion)
        , mScratch(scratch) {}

    [[nodiscard]] const World& GetWorld() const noexcept { return mWorld; }
    [[nodiscard]] CommandBuffer& GetCommands() noexcept { return mCommands; }
    [[nodiscard]] void* GetData() const noexcept { return mDesc.mData; }

    template <typename T>
    [[nodiscard]] T* GetData() const noexcept {
        return static_cast<T*>(mDesc.mData);
    }

    /// Version this run stamps its writes with.
    [[nodiscard]] uint32_t GetVersion() const noexcept { return mVersion; }
    /// Version of this system's previous run; `view.HasChangedSince(type, GetLastVersion())`
    /// is the change filter. Zero on the first run, so everything counts as changed.
    [[nodiscard]] uint32_t GetLastVersion() const noexcept { return mLastVersion; }
    /// Per-run scratch memory: the worker's frame allocator when running on the job system.
    [[nodiscard]] core::Allocator& GetScratch() const noexcept { return mScratch; }

    [[nodiscard]] bool CanRead(ComponentTypeIndex type) const noexcept {
        return mDesc.mRead.Test(type) || mDesc.mWrite.Test(type);
    }
    [[nodiscard]] bool CanWrite(ComponentTypeIndex type) const noexcept { return mDesc.mWrite.Test(type); }
    [[nodiscard]] bool CanSend(MessageTypeIndex type) const noexcept { return mDesc.mSends.Test(type); }
    [[nodiscard]] bool CanReceive(MessageTypeIndex type) const noexcept { return mDesc.mReceives.Test(type); }
    [[nodiscard]] bool CanReadResource(ResourceTypeIndex type) const noexcept {
        return mDesc.mReadResources.Test(type) || mDesc.mWriteResources.Test(type);
    }
    [[nodiscard]] bool CanWriteResource(ResourceTypeIndex type) const noexcept {
        return mDesc.mWriteResources.Test(type);
    }

    /// A resource (ADR-0007: Resource<T>); nullptr when none is registered under the index. The
    /// caller names the type it registered.
    template <typename T>
    [[nodiscard]] const T* GetResource(ResourceTypeIndex type) const noexcept {
        OYI_ASSERT(CanReadResource(type), "system reads a resource it did not declare");
        return mWorld.GetResources().Get<T>(type);
    }

    template <typename T>
    [[nodiscard]] T* GetResourceForWrite(ResourceTypeIndex type) noexcept {
        OYI_ASSERT(CanWriteResource(type), "system writes a resource it did not declare");
        return mWorld.GetResources().Get<T>(type);
    }

    [[nodiscard]] const Environment& GetEnvironment() const noexcept {
        OYI_ASSERT(CanReadResource(kEnvironmentResource), "system reads the Environment without declaring it");
        return mWorld.GetEnvironment();
    }

    [[nodiscard]] Environment& GetEnvironmentForWrite() noexcept {
        OYI_ASSERT(CanWriteResource(kEnvironmentResource), "system writes the Environment without declaring it");
        return mWorld.GetEnvironmentForWrite();
    }

    /// Queues a message for the receivers of `type` whose query matches `target` (every receiver
    /// when `target` is invalid). Delivered at the end of this phase, read on their next run.
    template <typename T>
    void Send(MessageTypeIndex type, EntityId target, const T& value) noexcept {
        OYI_ASSERT(CanSend(type), "system sends a message type it did not declare");
        mOutbox.Send(type, target, value);
    }

    /// The messages of `type` delivered since this system last ran.
    template <typename T>
    [[nodiscard]] ReceivedMessages<T> Received(MessageTypeIndex type) const noexcept {
        OYI_ASSERT(CanReceive(type), "system reads a message type it did not declare");
        return ReceivedMessages<T>{.mTargets = mInbox.Targets(type), .mPayloads = mInbox.Payloads<T>(type)};
    }

    /// Visits every chunk the system's query matches. `fn` receives a ChunkView&.
    template <typename Fn>
    void ForEachChunk(Fn&& fn) {
        mWorld.ForEachChunk(mQuery, mVersion, fn);
    }

    template <typename T>
    [[nodiscard]] std::span<const T> Read(const ChunkView& view, ComponentTypeIndex type) const noexcept {
        OYI_ASSERT(CanRead(type), "system reads a component type it did not declare");
        return view.Read<T>(type);
    }

    template <typename T>
    [[nodiscard]] std::span<T> Write(ChunkView& view, ComponentTypeIndex type) noexcept {
        OYI_ASSERT(CanWrite(type), "system writes a component type it did not declare");
        return view.Write<T>(type);
    }

    /// Random access by entity (ADR-0007: Query / ComponentLookup). Declared like any other
    /// access; nullptr for a dead entity or a missing component. Parallel-unfriendly: keep it to
    /// hierarchies and lookups, never to the hot loop over a chunk.
    template <typename T>
    [[nodiscard]] const T* Lookup(ComponentTypeIndex type, EntityId entity) const noexcept {
        OYI_ASSERT(CanRead(type), "system looks up a component type it did not declare");
        return static_cast<const T*>(mWorld.GetComponent(entity, type));
    }

    template <typename T>
    [[nodiscard]] T* LookupForWrite(ComponentTypeIndex type, EntityId entity) noexcept {
        OYI_ASSERT(CanWrite(type), "system writes a component type it did not declare");
        return static_cast<T*>(mWorld.GetComponentForWrite(entity, type, mVersion));
    }

private:
    World& mWorld;
    Query& mQuery;
    CommandBuffer& mCommands;
    MessageBuffer& mOutbox;
    const MessageInbox& mInbox;
    const SystemDesc& mDesc;
    uint32_t mVersion;
    uint32_t mLastVersion;
    core::Allocator& mScratch;
};

} // namespace oyi::ecs
