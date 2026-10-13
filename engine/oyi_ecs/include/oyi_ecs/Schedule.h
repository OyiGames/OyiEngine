#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/AccessLog.h>
#include <oyi_ecs/CommandBuffer.h>
#include <oyi_ecs/ComponentMask.h>
#include <oyi_ecs/Firewall.h>
#include <oyi_ecs/Message.h>
#include <oyi_ecs/Snapshot.h>
#include <oyi_ecs/System.h>
#include <oyi_ecs/World.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobSystem.h>

#include <array>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <vector>

namespace oyi::ecs {

/// The systems of one World, ordered per phase (ADR-0007).
///
/// Registration declares each system's access masks; AddOrder adds explicit edges. Build derives
/// the dependency graph once: every pair of systems in the same phase that conflicts (one writes
/// what the other reads or writes) must be ordered by an explicit path, otherwise Build fails,
/// because an unordered conflict pair would run in a timing-dependent order. Non-conflicting
/// systems run side by side. Each phase is one instantiation of the static graph on the job
/// system followed by a sync point; RunPhaseSerial runs the same order on the calling thread and
/// must produce the same result (the single-thread invariant).
class Schedule {
public:
    struct Conflict {
        SystemId mFirst;
        SystemId mSecond;
    };

    /// ADR-0019: what the runtime access checks do with what the systems recorded.
    enum class AccessCheckMode : uint8_t {
        Off,    ///< nothing is recorded
        Record, ///< violations are collected (GetViolations) and execution goes on
        Assert, ///< a violation terminates, as a failed OYI_CHECK does
    };

    enum class AccessViolationKind : uint8_t {
        Undeclared, ///< a system touched a type outside its declared masks
        Unordered,  ///< two systems without an order edge touched the same chunk and one wrote
        CrossChunk, ///< a chunk task touched another chunk for a type its system writes (Premark)
    };

    struct AccessViolation {
        AccessViolationKind mKind;
        SystemId mFirst;
        SystemId mSecond; ///< invalid for Undeclared
        ComponentTypeIndex mType;
        AccessKind mAccess;
        const Archetype* mArchetype;
        uint32_t mChunk;
    };

    Schedule(core::Allocator& allocator, World& world) noexcept;
    Schedule(const Schedule&)            = delete;
    Schedule(Schedule&&)                 = delete;
    Schedule& operator=(const Schedule&) = delete;
    Schedule& operator=(Schedule&&)      = delete;
    ~Schedule();

    /// Fails with InvalidState after Build, InvalidArgument for an unnamed system. `kinds` says
    /// which worlds the system belongs to (ADR-0017: a server world leaves presentation systems
    /// out); Build drops systems whose kinds do not include the world's.
    [[nodiscard]] core::Expected<SystemId> AddSystem(const SystemDesc& desc,
                                                     WorldKindMask kinds = kAllWorldKinds) noexcept;
    /// False for a system Build left out because of its kinds.
    [[nodiscard]] bool IsSystemIncluded(SystemId system) const noexcept;

    /// Runs the system every `divisor`th frame (ADR-0007: presentation systems may be downclocked
    /// per device profile). Only systems of the Presentation and End phases qualify; anything
    /// earlier affects the simulation and fails with InvalidArgument, as does a divisor of zero.
    /// A skipped run keeps its change-filter version and its inbox, so the next run sees
    /// everything that happened meanwhile. May be changed after Build.
    [[nodiscard]] core::Result SetRateDivisor(SystemId system, uint32_t divisor) noexcept;
    [[nodiscard]] uint32_t GetRateDivisor(SystemId system) const noexcept;
    /// `before` runs to completion before `after` starts. Both must be in the same phase.
    [[nodiscard]] core::Result AddOrder(SystemId before, SystemId after) noexcept;
    /// Derives the per-phase order. Fails with InvalidState on an unordered conflict pair (see
    /// GetLastConflict) or a cycle.
    [[nodiscard]] core::Result Build() noexcept;
    [[nodiscard]] bool IsBuilt() const noexcept { return mBuilt; }
    [[nodiscard]] Conflict GetLastConflict() const noexcept { return mLastConflict; }

    [[nodiscard]] uint32_t GetSystemCount() const noexcept { return static_cast<uint32_t>(mSystems.size()); }
    [[nodiscard]] const SystemDesc& GetDesc(SystemId system) const noexcept;
    /// Systems of `phase` in execution order (a topological order with registration order as
    /// the tie-breaker). Empty before Build.
    [[nodiscard]] std::span<const SystemId> GetOrder(Phase phase) const noexcept;
    /// Direct predecessors of a system: the AddOrder edges and, after Build, the edges added for
    /// automatically ordered resources.
    [[nodiscard]] std::span<const SystemId> GetPredecessors(SystemId system) const noexcept;

    /// Runs the phase's systems on the job system (the calling thread must be the one that built
    /// the JobSystem; it helps while it waits), then replays their command buffers in system
    /// order and advances the world version.
    [[nodiscard]] core::Result RunPhase(Phase phase, job::JobSystem& jobs) noexcept;
    /// The same phase on the calling thread, one system after another: the reference behaviour
    /// for the single-thread invariant and the mode to validate a new system in first.
    [[nodiscard]] core::Result RunPhaseSerial(Phase phase) noexcept;
    [[nodiscard]] core::Result RunFrame(job::JobSystem& jobs) noexcept;
    [[nodiscard]] core::Result RunFrameSerial() noexcept;

    [[nodiscard]] World::ReplayStats GetLastReplayStats() const noexcept { return mLastReplay; }
    /// Commands replayed when the last firewall window closed.
    [[nodiscard]] World::ReplayStats GetLastWindowReplayStats() const noexcept { return mLastWindowReplay; }

    /// Runtime access checks (ADR-0019, MAW-lite): every system run records the (chunk, type)
    /// pairs it touched; at the phase boundary each record is checked against the declared masks
    /// and every unordered pair of systems is checked for overlapping writes. Assert in Debug
    /// builds, Off when OYI_ACCESS_CHECKS is compiled out; the serial run checks the same way.
    void SetAccessCheckMode(AccessCheckMode mode) noexcept { mAccessMode = mode; }
    [[nodiscard]] AccessCheckMode GetAccessCheckMode() const noexcept { return mAccessMode; }
    /// Violations found at the end of the last phase run.
    [[nodiscard]] std::span<const AccessViolation> GetViolations() const noexcept { return mViolations; }
    /// What `system` touched during its last run.
    [[nodiscard]] const AccessLog& GetAccessLog(SystemId system) const noexcept;

    /// Captures `types` into `snapshot` right after the sync point of `phase`, every time that
    /// phase runs (ADR-0006: the Prev snapshot the managed layer reads). nullptr disables it.
    void SetSnapshotCapture(Snapshot* snapshot, const ComponentMask& types, Phase phase) noexcept;

    /// Opens a firewall window (ADR-0006) after the sync point of `phase` (and after the snapshot
    /// capture, when it is on the same phase), every time that phase runs. One window per phase;
    /// nullptr removes it.
    void SetWindow(Phase phase, WindowFunction function, void* data) noexcept;
    /// Opens a window now, on the calling thread: no job may be in flight. The commands and
    /// messages it records are applied and routed when `function` returns, before this call
    /// returns (the window's own sync point). Fails with InvalidArgument without a function.
    [[nodiscard]] core::Result RunWindow(WindowFunction function, void* data) noexcept;

private:
    struct Window {
        WindowFunction mFunction = nullptr;
        void* mData              = nullptr;
    };

    struct SystemRecord;
    struct TaskOwner;
    struct ChunkTask;

    static void RunSystemJob(job::JobContext& context, void* data);
    [[nodiscard]] static core::Result DispatchChunkTasks(void* owner, SystemContext& outer,
                                                         ChunkTaskFunction function) noexcept;
    static void RunChunkTask(SystemRecord& record, const ChunkTask& task, core::Allocator& scratch) noexcept;
    static void RunChunkTasksJob(job::JobContext& context, void* data, uint32_t begin, uint32_t end);
    /// Derives the per-phase order from the current edges and fills `reach` (count x count).
    [[nodiscard]] core::Result Sort(std::span<uint8_t> reach) noexcept;
    void RunSystem(SystemRecord& record, uint32_t version, core::Allocator& scratch) noexcept;
    [[nodiscard]] core::Result EndPhase(Phase phase, uint32_t versionsUsed) noexcept;
    void RouteMessages(Phase phase) noexcept;
    void RouteOutbox(MessageBuffer& outbox) noexcept;
    void ValidateAccesses(Phase phase) noexcept;
    void RefreshQueries(Phase phase) noexcept;

    core::Allocator& mAllocator;
    World& mWorld;
    core::MemoryResource mResource;
    std::pmr::vector<SystemRecord*> mSystems;
    std::array<std::pmr::vector<SystemId>, kPhaseCount> mOrder;
    std::pmr::vector<CommandBuffer*> mPhaseBuffers; ///< scratch for the sync point
    bool mBuilt = false;
    Conflict mLastConflict;
    World::ReplayStats mLastReplay;
    World::ReplayStats mLastWindowReplay;
    CommandBuffer mWindowCommands;
    MessageBuffer mWindowOutbox;
    std::array<Window, kPhaseCount> mWindows{};
    std::pmr::vector<uint8_t> mReach; ///< count x count: a is ordered before b, kept from Build
    std::pmr::vector<AccessViolation> mViolations;
    AccessCheckMode mAccessMode = OYI_ACCESS_CHECKS != 0 ? AccessCheckMode::Assert : AccessCheckMode::Off;
    Snapshot* mSnapshot         = nullptr;
    ComponentMask mSnapshotTypes;
    Phase mSnapshotPhase = Phase::End;
};

} // namespace oyi::ecs
