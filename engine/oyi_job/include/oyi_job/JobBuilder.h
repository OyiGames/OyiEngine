#pragma once

#include <oyi_core/Expected.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobSystem.h>

#include <cstdint>
#include <string_view>

namespace oyi::job {

/// Stack-scoped front end over JobSystem::RunJob (ADR-0009). Jobs dispatched through one builder
/// share its priority and are serial by default: each waits for the previous one. Parallel is an
/// explicit choice. The builder never blocks; the one way to wait is to hand a counter from
/// ExtractWaitCounter to another builder, a job, or JobSystem::WaitForCounter.
///
/// A builder created with a parent counter behaves as one job of the parent: the parent stays
/// pending from construction until everything dispatched through the builder has finished, which
/// the destructor arranges with an empty splice job. Inside a job, pass `context.mCompletion` as
/// the parent so that child jobs extend the job's completion.
///
/// Counters passed in (`waitFor`, `parent`, DispatchWait) must stay valid for the builder's life.
class JobBuilder {
public:
    enum class Fence : uint8_t {
        Full, ///< every job waits for the previous one (default)
        None, ///< jobs only wait for the builder's entry dependency and run side by side
    };

    /// `name` labels the splice jobs; `waitFor` is what the first job waits for (invalid for
    /// nothing); `parent` is spliced into at destruction (invalid for a fire-and-forget builder).
    JobBuilder(JobSystem& system, std::string_view name, JobPriority priority, CounterId waitFor, CounterId parent,
               Fence fence) noexcept;
    /// Serial, no dependency, no parent.
    JobBuilder(JobSystem& system, std::string_view name, JobPriority priority) noexcept;
    JobBuilder(const JobBuilder&)            = delete;
    JobBuilder(JobBuilder&&)                 = delete;
    JobBuilder& operator=(const JobBuilder&) = delete;
    JobBuilder& operator=(JobBuilder&&)      = delete;
    /// Splices the builder's completion into the parent counter, when there is one.
    ~JobBuilder();

    /// Dispatches one job. Fails only when a pool is exhausted; the chain stays intact.
    [[nodiscard]] core::Result DispatchJob(std::string_view name, JobFunction function, void* data) noexcept;

    /// Makes every later job also wait for `counter`, without blocking.
    [[nodiscard]] core::Result DispatchWait(CounterId counter) noexcept;

    /// A counter that reaches zero when everything dispatched so far has finished. The caller owns
    /// it and must ReleaseCounter it. Invalid when a pool is exhausted.
    [[nodiscard]] CounterId ExtractWaitCounter() noexcept;

    [[nodiscard]] JobSystem& GetSystem() const noexcept { return mSystem; }
    [[nodiscard]] JobPriority GetPriority() const noexcept { return mPriority; }
    [[nodiscard]] Fence GetFence() const noexcept { return mFence; }
    [[nodiscard]] std::string_view GetName() const noexcept { return mName; }
    [[nodiscard]] uint32_t GetDispatchedCount() const noexcept { return mDispatched; }

    /// What a newly dispatched job waits for right now; ParallelFor dispatches against it.
    [[nodiscard]] CounterId GetEntry() const noexcept { return mEntry; }
    /// Records a step whose completion is `completion`, which the caller created and hands over.
    /// ParallelFor uses this after dispatching its chunks and epilogue.
    void AdvanceChain(CounterId completion) noexcept;

private:
    [[nodiscard]] CounterId CompletionSoFar() const noexcept;
    [[nodiscard]] core::Result Splice(CounterId waitFor, CounterId accumulate) noexcept;
    void SetEntry(CounterId entry, bool owned) noexcept;
    void SetTail(CounterId tail) noexcept;

    JobSystem& mSystem;
    std::string_view mName;
    JobPriority mPriority;
    Fence mFence;
    CounterId mParent;
    CounterId mEntry;        ///< what the next job waits for
    bool mOwnsEntry = false; ///< whether mEntry was created by the builder (and is not mTail)
    CounterId mTail;         ///< owned; serial: the last job's completion, parallel: the shared accumulate target
    uint32_t mDispatched = 0;
};

} // namespace oyi::job
