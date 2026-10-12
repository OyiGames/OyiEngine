#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_ecs/ComponentMask.h>
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

    Schedule(core::Allocator& allocator, World& world) noexcept;
    Schedule(const Schedule&)            = delete;
    Schedule(Schedule&&)                 = delete;
    Schedule& operator=(const Schedule&) = delete;
    Schedule& operator=(Schedule&&)      = delete;
    ~Schedule();

    /// Fails with InvalidState after Build, InvalidArgument for an unnamed system.
    [[nodiscard]] core::Expected<SystemId> AddSystem(const SystemDesc& desc) noexcept;
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
    /// Direct predecessors of a system from AddOrder.
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

    /// Captures `types` into `snapshot` right after the sync point of `phase`, every time that
    /// phase runs (ADR-0006: the Prev snapshot the managed layer reads). nullptr disables it.
    void SetSnapshotCapture(Snapshot* snapshot, const ComponentMask& types, Phase phase) noexcept;

private:
    struct SystemRecord;

    static void RunSystemJob(job::JobContext& context, void* data);
    void RunSystem(SystemRecord& record, uint32_t version, core::Allocator& scratch) noexcept;
    [[nodiscard]] core::Result EndPhase(Phase phase, uint32_t versionsUsed) noexcept;
    void RouteMessages(Phase phase) noexcept;
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
    Snapshot* mSnapshot = nullptr;
    ComponentMask mSnapshotTypes;
    Phase mSnapshotPhase = Phase::End;
};

} // namespace oyi::ecs
