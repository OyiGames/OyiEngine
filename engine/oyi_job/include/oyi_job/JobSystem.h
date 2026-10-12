#pragma once

#include <oyi_core/Allocator.h>
#include <oyi_core/Expected.h>
#include <oyi_core/FrameAllocator.h>
#include <oyi_core/Memory.h>
#include <oyi_core/MemoryResource.h>
#include <oyi_core/TaggedHeap.h>
#include <oyi_job/Job.h>

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <mutex>
#include <thread>
#include <vector>

namespace oyi::job {

/// Worker threads, per-priority FIFO queues and the counter pool (ADR-0009).
///
/// Jobs run to completion and never suspend; waiting lives on graph edges: a job registered with
/// `waitFor` is queued only when that counter reaches zero. The main thread may wait, and while it
/// waits it executes ready Critical jobs itself. There is no work stealing and no preemption.
///
/// Counters are reference counted: the creator holds one reference until ReleaseCounter, and every
/// pending completion (an accumulating job or a CompletionDeferral) holds one more, so a counter
/// may be released while jobs still accumulate into it. A counter that has no pending completions
/// reads as zero: register every accumulating job (or hold a CompletionDeferral) before anyone
/// waits on it.
class JobSystem {
public:
    struct Config {
        uint32_t mWorkerCount    = 0;    ///< 0 picks hardware threads minus one, at least one
        uint32_t mMaxJobs        = 4096; ///< jobs queued, waiting or running at the same time
        uint32_t mMaxCounters    = 1024;
        uint32_t mSpinIterations = 64; ///< yields before a thread falls back to a system wait
        core::TaggedHeap::Config mFrameHeap{};
    };

    struct Stats {
        uint32_t mWorkerCount  = 0;
        uint32_t mInFlightJobs = 0; ///< queued, waiting or running
    };

    /// Uses the default Config.
    explicit JobSystem(core::Allocator& allocator);
    JobSystem(core::Allocator& allocator, Config config);
    JobSystem(const JobSystem&)            = delete;
    JobSystem(JobSystem&&)                 = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem& operator=(JobSystem&&)      = delete;
    /// Stops the workers. Nothing may be in flight: drain with WaitForCounter first.
    ~JobSystem();

    // --- counters -------------------------------------------------------------------------------

    /// A fresh counter at zero; invalid when the pool is exhausted.
    [[nodiscard]] CounterId CreateCounter() noexcept;
    /// Drops the creator's reference. The slot is recycled once no completion is pending.
    void ReleaseCounter(CounterId counter) noexcept;
    [[nodiscard]] bool IsCounterValid(CounterId counter) const noexcept;
    [[nodiscard]] bool IsCounterZero(CounterId counter) const noexcept;
    /// Registers a pending completion from outside the job graph (a loading thread, an I/O
    /// callback). Prefer CompletionDeferral, which pairs the two calls.
    void IncrementCounter(CounterId counter) noexcept;
    void DecrementCounter(CounterId counter) noexcept;

    // --- jobs -----------------------------------------------------------------------------------

    /// The one dispatch primitive. The job runs after `waitFor` reaches zero (or at once when it
    /// is invalid) and counts down `accumulate` when it finishes (ignored when invalid). Fails with
    /// OutOfMemory when mMaxJobs are in flight; nothing is registered in that case.
    [[nodiscard]] core::Result RunJob(const JobDesc& desc, CounterId waitFor, CounterId accumulate) noexcept;

    /// Blocks the calling thread until `counter` is zero. The main thread (the one that built the
    /// JobSystem) runs ready Critical jobs instead of sleeping. Must not be called from a job.
    void WaitForCounter(CounterId counter) noexcept;

    /// True on a thread that is currently executing a job.
    [[nodiscard]] static bool IsInJob() noexcept;

    // --- frame memory ---------------------------------------------------------------------------

    /// Points every worker's frame allocator at `tag`. Call at a frame boundary with nothing in
    /// flight.
    void BeginFrame(core::MemoryTag tag) noexcept;
    /// Releases all frame memory allocated under `tag`, once every job of that frame has finished.
    void FreeFrame(core::MemoryTag tag) noexcept;
    /// Frame memory for main-thread code; same tag discipline as the workers.
    [[nodiscard]] core::FrameAllocator& GetMainFrameAllocator() noexcept;
    [[nodiscard]] core::TaggedHeap::Stats GetFrameHeapStats() const noexcept;

    [[nodiscard]] Stats GetStats() const noexcept;

private:
    struct JobRecord;
    struct CounterSlot;
    struct Worker;
    struct Queue {
        JobRecord* mHead = nullptr;
        JobRecord* mTail = nullptr;
    };

    void WorkerMain(uint32_t workerIndex) noexcept;
    [[nodiscard]] JobRecord* TryPop(JobPriority lowest) noexcept;
    [[nodiscard]] bool HasQueuedLocked() const noexcept;
    void Execute(JobRecord* job, Worker& worker) noexcept;
    void Complete(JobRecord* job) noexcept;
    void DispatchReady(JobRecord* job) noexcept;
    void ReleaseWaiters(CounterSlot& slot) noexcept;
    [[nodiscard]] CounterSlot& GetSlot(CounterId counter) noexcept;
    [[nodiscard]] const CounterSlot& GetSlot(CounterId counter) const noexcept;
    static void AddCounterRef(CounterSlot& slot) noexcept;
    void DropCounterRef(CounterSlot& slot) noexcept;
    [[nodiscard]] JobRecord* AllocateJob() noexcept;
    void FreeJob(JobRecord* job) noexcept;

    core::Allocator& mAllocator;
    Config mConfig;
    core::TaggedHeap mFrameHeap;
    core::MemoryResource mResource;
    std::thread::id mMainThreadId;

    JobRecord* mJobs                = nullptr;
    JobRecord* mFreeJobs            = nullptr;
    CounterSlot* mCounters          = nullptr;
    uint32_t mFreeCounterHead       = 0;
    Worker* mWorkers                = nullptr; ///< mConfig.mWorkerCount workers plus one slot for the main thread
    std::atomic<uint32_t> mInFlight = 0;
    std::atomic<bool> mStop         = false;

    mutable std::mutex mMutex; ///< queues, free lists and the thread-wait condition
    std::condition_variable mWorkerCv;
    std::condition_variable mWaitCv;
    Queue mQueues[kJobPriorityCount];
    std::pmr::vector<std::thread> mThreads;
};

} // namespace oyi::job
