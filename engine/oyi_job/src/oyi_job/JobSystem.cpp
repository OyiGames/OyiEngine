#include <oyi_core/Allocator.h>
#include <oyi_core/Assert.h>
#include <oyi_core/Expected.h>
#include <oyi_core/FrameAllocator.h>
#include <oyi_core/Memory.h>
#include <oyi_core/Profile.h>
#include <oyi_core/SpinLock.h>
#include <oyi_core/TaggedHeap.h>
#include <oyi_job/Job.h>
#include <oyi_job/JobSystem.h>
#include <oyi_platform/Thread.h>
#include <oyi_platform/Topology.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <system_error>
#include <thread>

namespace oyi::job {

namespace {

constexpr uint32_t kNoSlot = 0xFFFF'FFFFu;

/// Set while a thread executes a job; the one piece of thread-local state in the scheduler.
thread_local const JobContext* gCurrentContext = nullptr;

template <typename T>
[[nodiscard]] T* AllocateArray(core::Allocator& allocator, size_t count) noexcept {
    void* memory = allocator.Allocate(sizeof(T) * count, alignof(T));
    OYI_CHECK(memory != nullptr, "JobSystem: out of memory while building the pools");
    auto* array = static_cast<T*>(memory);
    for (size_t i = 0; i < count; ++i) {
        std::construct_at(array + i);
    }
    return array;
}

template <typename T>
void FreeArray(core::Allocator& allocator, T* array, size_t count) noexcept {
    if (array == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        std::destroy_at(array + i);
    }
    allocator.Free(array, sizeof(T) * count, alignof(T));
}

} // namespace

/// One in-flight job. Lives in exactly one place at a time: the free list, a counter's wait list,
/// a priority queue, or a worker.
struct JobSystem::JobRecord {
    JobDesc mDesc{};
    CounterId mWaitFor;
    CounterId mAccumulate;
    JobRecord* mNext = nullptr;
};

struct JobSystem::CounterSlot {
    std::atomic<uint32_t> mValue      = 0; ///< pending completions
    std::atomic<uint32_t> mRefs       = 0; ///< creator + pending completions; zero means free
    std::atomic<uint32_t> mGeneration = 1; ///< never zero: generation zero is the invalid handle
    uint32_t mNextFree                = kNoSlot;
    core::SpinLock mLock; ///< guards the wait list and mThreadWaiting
    JobRecord* mWaitersHead = nullptr;
    JobRecord* mWaitersTail = nullptr;
    bool mThreadWaiting     = false;
};

struct JobSystem::Worker {
    Worker(core::TaggedHeap& heap, uint32_t index) noexcept : mFrameAllocator(heap, core::MemoryTag{}), mIndex(index) {}

    core::FrameAllocator mFrameAllocator;
    uint32_t mIndex;
};

JobSystem::JobSystem(core::Allocator& allocator) : JobSystem(allocator, Config{}) {}

JobSystem::JobSystem(core::Allocator& allocator, Config config)
    : mAllocator(allocator)
    , mConfig(config)
    , mFrameHeap(allocator, config.mFrameHeap)
    , mResource(allocator)
    , mMainThreadId(std::this_thread::get_id())
    , mThreads(&mResource) {
    if (mConfig.mWorkerCount == 0) {
        // ADR-0009: workers are counted from the big cores; the main thread takes one of them.
        mConfig.mWorkerCount = platform::RecommendedWorkerCount(platform::QueryCoreTopology());
    }
    OYI_CHECK(mConfig.mMaxJobs > 0, "JobSystem: the job pool must not be empty");
    OYI_CHECK(mConfig.mMaxCounters > 0, "JobSystem: the counter pool must not be empty");

    mJobs = AllocateArray<JobRecord>(mAllocator, mConfig.mMaxJobs);
    for (uint32_t i = 0; i < mConfig.mMaxJobs; ++i) {
        mJobs[i].mNext = i + 1 < mConfig.mMaxJobs ? &mJobs[i + 1] : nullptr;
    }
    mFreeJobs = mJobs;

    mCounters = AllocateArray<CounterSlot>(mAllocator, mConfig.mMaxCounters);
    for (uint32_t i = 0; i < mConfig.mMaxCounters; ++i) {
        mCounters[i].mNextFree = i + 1 < mConfig.mMaxCounters ? i + 1 : kNoSlot;
    }
    mFreeCounterHead = 0;

    const uint32_t workerSlots = mConfig.mWorkerCount + 1;
    void* workerMemory         = mAllocator.Allocate(sizeof(Worker) * workerSlots, alignof(Worker));
    OYI_CHECK(workerMemory != nullptr, "JobSystem: out of memory while building the workers");
    mWorkers = static_cast<Worker*>(workerMemory);
    for (uint32_t i = 0; i < workerSlots; ++i) {
        std::construct_at(mWorkers + i, mFrameHeap, i);
    }

    mThreads.reserve(mConfig.mWorkerCount);
    for (uint32_t i = 0; i < mConfig.mWorkerCount; ++i) {
        mThreads.emplace_back([this, i] { WorkerMain(i); });
    }
}

JobSystem::~JobSystem() {
    OYI_CHECK(mInFlight.load(std::memory_order_acquire) == 0, "JobSystem destroyed with jobs in flight");
    {
        const std::scoped_lock lock(mMutex);
        mStop.store(true, std::memory_order_release);
    }
    mWorkerCv.notify_all();
    for (std::thread& thread : mThreads) {
        thread.join();
    }

    const uint32_t workerSlots = mConfig.mWorkerCount + 1;
    for (uint32_t i = 0; i < workerSlots; ++i) {
        std::destroy_at(mWorkers + i);
    }
    mAllocator.Free(mWorkers, sizeof(Worker) * workerSlots, alignof(Worker));
    FreeArray(mAllocator, mCounters, mConfig.mMaxCounters);
    FreeArray(mAllocator, mJobs, mConfig.mMaxJobs);
}

// --- counters ---------------------------------------------------------------------------------

JobSystem::CounterSlot& JobSystem::GetSlot(CounterId counter) noexcept {
    OYI_ASSERT(IsCounterValid(counter), "stale or invalid CounterId");
    return mCounters[counter.mIndex];
}

const JobSystem::CounterSlot& JobSystem::GetSlot(CounterId counter) const noexcept {
    OYI_ASSERT(IsCounterValid(counter), "stale or invalid CounterId");
    return mCounters[counter.mIndex];
}

CounterId JobSystem::CreateCounter() noexcept {
    const std::scoped_lock lock(mMutex);
    if (mFreeCounterHead == kNoSlot) {
        return CounterId::Invalid();
    }
    const uint32_t index = mFreeCounterHead;
    CounterSlot& slot    = mCounters[index];
    mFreeCounterHead     = slot.mNextFree;
    slot.mNextFree       = kNoSlot;
    slot.mValue.store(0, std::memory_order_relaxed);
    slot.mRefs.store(1, std::memory_order_release);
    return CounterId{.mIndex = index, .mGeneration = slot.mGeneration.load(std::memory_order_relaxed)};
}

void JobSystem::ReleaseCounter(CounterId counter) noexcept {
    DropCounterRef(GetSlot(counter));
}

bool JobSystem::IsCounterValid(CounterId counter) const noexcept {
    if (!counter.IsValid() || counter.mIndex >= mConfig.mMaxCounters) {
        return false;
    }
    const CounterSlot& slot = mCounters[counter.mIndex];
    return slot.mGeneration.load(std::memory_order_acquire) == counter.mGeneration &&
           slot.mRefs.load(std::memory_order_acquire) != 0;
}

bool JobSystem::IsCounterZero(CounterId counter) const noexcept {
    return GetSlot(counter).mValue.load(std::memory_order_acquire) == 0;
}

void JobSystem::AddCounterRef(CounterSlot& slot) noexcept {
    slot.mRefs.fetch_add(1, std::memory_order_relaxed);
}

void JobSystem::DropCounterRef(CounterSlot& slot) noexcept {
    if (slot.mRefs.fetch_sub(1, std::memory_order_acq_rel) != 1) {
        return;
    }
    // Last reference: nothing is pending and nobody can wait any more, recycle the slot.
    OYI_ASSERT(slot.mValue.load(std::memory_order_relaxed) == 0, "counter recycled while pending");
    OYI_ASSERT(slot.mWaitersHead == nullptr, "counter recycled with waiting jobs");
    const uint32_t next = slot.mGeneration.load(std::memory_order_relaxed) + 1;
    slot.mGeneration.store(next == 0 ? 1 : next, std::memory_order_release);
    const std::scoped_lock lock(mMutex);
    slot.mNextFree   = mFreeCounterHead;
    mFreeCounterHead = static_cast<uint32_t>(&slot - mCounters);
}

void JobSystem::IncrementCounter(CounterId counter) noexcept {
    CounterSlot& slot = GetSlot(counter);
    AddCounterRef(slot);
    slot.mValue.fetch_add(1, std::memory_order_relaxed);
}

void JobSystem::DecrementCounter(CounterId counter) noexcept {
    CounterSlot& slot     = GetSlot(counter);
    const uint32_t before = slot.mValue.fetch_sub(1, std::memory_order_acq_rel);
    OYI_ASSERT(before != 0, "counter decremented below zero");
    if (before == 1) {
        ReleaseWaiters(slot);
    }
    DropCounterRef(slot);
}

void JobSystem::ReleaseWaiters(CounterSlot& slot) noexcept {
    JobRecord* waiters = nullptr;
    bool threadWaiting = false;
    {
        const core::SpinLockGuard guard(slot.mLock);
        waiters             = slot.mWaitersHead;
        slot.mWaitersHead   = nullptr;
        slot.mWaitersTail   = nullptr;
        threadWaiting       = slot.mThreadWaiting;
        slot.mThreadWaiting = false;
    }
    while (waiters != nullptr) {
        JobRecord* const next = waiters->mNext;
        waiters->mNext        = nullptr;
        DispatchReady(waiters);
        waiters = next;
    }
    if (threadWaiting) {
        // Taking the mutex orders this notification after the waiter's predicate check, so a
        // waiter that saw a non-zero value is guaranteed to be waiting when notify_all runs.
        {
            const std::scoped_lock lock(mMutex);
        }
        mWaitCv.notify_all();
    }
}

// --- jobs -------------------------------------------------------------------------------------

JobSystem::JobRecord* JobSystem::AllocateJob() noexcept {
    const std::scoped_lock lock(mMutex);
    if (mFreeJobs == nullptr) {
        return nullptr;
    }
    JobRecord* const job = mFreeJobs;
    mFreeJobs            = job->mNext;
    job->mNext           = nullptr;
    mInFlight.fetch_add(1, std::memory_order_relaxed);
    return job;
}

void JobSystem::FreeJob(JobRecord* job) noexcept {
    const std::scoped_lock lock(mMutex);
    job->mDesc = JobDesc{};
    job->mNext = mFreeJobs;
    mFreeJobs  = job;
    mInFlight.fetch_sub(1, std::memory_order_release);
}

core::Result JobSystem::RunJob(const JobDesc& desc, CounterId waitFor, CounterId accumulate) noexcept {
    OYI_ASSERT(!desc.mName.empty(), "every job needs a name");
    JobRecord* const job = AllocateJob();
    if (job == nullptr) {
        return core::Fail(core::ErrorCode::OutOfMemory, "JobSystem: job pool exhausted");
    }
    job->mDesc       = desc;
    job->mWaitFor    = waitFor;
    job->mAccumulate = accumulate;

    if (accumulate.IsValid()) {
        CounterSlot& slot = GetSlot(accumulate);
        AddCounterRef(slot);
        slot.mValue.fetch_add(1, std::memory_order_relaxed);
    }

    if (waitFor.IsValid()) {
        CounterSlot& slot = GetSlot(waitFor);
        const core::SpinLockGuard guard(slot.mLock);
        // Checked under the lock: a decrement that reaches zero takes the same lock to collect
        // the waiters, so a job linked here is never missed.
        if (slot.mValue.load(std::memory_order_acquire) != 0) {
            if (slot.mWaitersTail != nullptr) {
                slot.mWaitersTail->mNext = job;
            } else {
                slot.mWaitersHead = job;
            }
            slot.mWaitersTail = job;
            return {};
        }
    }
    DispatchReady(job);
    return {};
}

void JobSystem::DispatchReady(JobRecord* job) noexcept {
    if (job->mDesc.mFunction == nullptr) {
        // Empty jobs only splice counters; they complete in place and never enter a queue.
        Complete(job);
        return;
    }
    {
        const std::scoped_lock lock(mMutex);
        Queue& queue = mQueues[static_cast<size_t>(job->mDesc.mPriority)];
        if (queue.mTail != nullptr) {
            queue.mTail->mNext = job;
        } else {
            queue.mHead = job;
        }
        queue.mTail = job;
    }
    mWorkerCv.notify_one();
}

bool JobSystem::HasQueuedLocked() const noexcept {
    return std::ranges::any_of(mQueues, [](const Queue& queue) { return queue.mHead != nullptr; });
}

JobSystem::JobRecord* JobSystem::TryPop(JobPriority lowest) noexcept {
    const std::scoped_lock lock(mMutex);
    const auto last = static_cast<size_t>(lowest);
    for (size_t priority = 0; priority <= last; ++priority) {
        Queue& queue = mQueues[priority];
        if (queue.mHead == nullptr) {
            continue;
        }
        JobRecord* const job = queue.mHead;
        queue.mHead          = job->mNext;
        if (queue.mHead == nullptr) {
            queue.mTail = nullptr;
        }
        job->mNext = nullptr;
        return job;
    }
    return nullptr;
}

void JobSystem::Execute(JobRecord* job, Worker& worker) noexcept {
    JobContext context{.mWorkerIndex    = worker.mIndex,
                       .mFrameAllocator = worker.mFrameAllocator,
                       .mJobName        = job->mDesc.mName,
                       .mCompletion     = job->mAccumulate};
    const JobContext* const previous = gCurrentContext;
    gCurrentContext                  = &context;
    {
        OYI_ZONE_TEXT(job->mDesc.mName); // ADR-0009: the job name is the marker
        job->mDesc.mFunction(context, job->mDesc.mData);
    }
    gCurrentContext = previous;
    Complete(job);
}

void JobSystem::Complete(JobRecord* job) noexcept {
    const CounterId accumulate = job->mAccumulate;
    // The record goes back first so that a waiter released by the decrement below never observes
    // this job as still in flight.
    FreeJob(job);
    if (accumulate.IsValid()) {
        DecrementCounter(accumulate);
    }
}

void JobSystem::WorkerMain(uint32_t workerIndex) noexcept {
    {
        // "oyi.worker.N" for debuggers and profilers.
        std::array<char, 24> name{'o', 'y', 'i', '.', 'w', 'o', 'r', 'k', 'e', 'r', '.'};
        constexpr size_t kPrefix = 11;
        const auto [end, error]  = std::to_chars(name.data() + kPrefix, name.data() + name.size(), workerIndex);
        const size_t length      = error == std::errc{} ? static_cast<size_t>(end - name.data()) : kPrefix;
        platform::SetCurrentThreadName(std::string_view(name.data(), length));
        OYI_THREAD_NAME(std::string_view(name.data(), length));
    }
    Worker& worker = mWorkers[workerIndex];
    while (true) {
        JobRecord* job = TryPop(JobPriority::Latent);
        for (uint32_t spin = 0; job == nullptr && spin < mConfig.mSpinIterations; ++spin) {
            std::this_thread::yield();
            job = TryPop(JobPriority::Latent);
        }
        if (job != nullptr) {
            Execute(job, worker);
            continue;
        }
        std::unique_lock lock(mMutex);
        mWorkerCv.wait(lock, [this] { return mStop.load(std::memory_order_acquire) || HasQueuedLocked(); });
        if (mStop.load(std::memory_order_acquire) && !HasQueuedLocked()) {
            return;
        }
    }
}

void JobSystem::WaitForCounter(CounterId counter) noexcept {
    OYI_CHECK(!IsInJob(), "WaitForCounter inside a job: jobs run to completion, waits belong on graph edges");
    CounterSlot& slot = GetSlot(counter);
    const bool help   = std::this_thread::get_id() == mMainThreadId;
    Worker& mainSlot  = mWorkers[mConfig.mWorkerCount];

    while (slot.mValue.load(std::memory_order_acquire) != 0) {
        if (help) {
            if (JobRecord* const job = TryPop(JobPriority::Critical); job != nullptr) {
                Execute(job, mainSlot);
                continue;
            }
        }
        for (uint32_t spin = 0; spin < mConfig.mSpinIterations; ++spin) {
            if (slot.mValue.load(std::memory_order_acquire) == 0) {
                return;
            }
            std::this_thread::yield();
        }
        {
            const core::SpinLockGuard guard(slot.mLock);
            if (slot.mValue.load(std::memory_order_acquire) == 0) {
                return;
            }
            slot.mThreadWaiting = true;
        }
        std::unique_lock lock(mMutex);
        if (help && HasQueuedLocked()) {
            continue;
        }
        mWaitCv.wait(lock, [&slot] { return slot.mValue.load(std::memory_order_acquire) == 0; });
    }
}

bool JobSystem::IsInJob() noexcept {
    return gCurrentContext != nullptr;
}

// --- frame memory -----------------------------------------------------------------------------

void JobSystem::BeginFrame(core::MemoryTag tag) noexcept {
    OYI_FRAME_MARK;
    OYI_CHECK(mInFlight.load(std::memory_order_acquire) == 0, "BeginFrame with jobs in flight");
    const uint32_t workerSlots = mConfig.mWorkerCount + 1;
    for (uint32_t i = 0; i < workerSlots; ++i) {
        mWorkers[i].mFrameAllocator.SetTag(tag);
    }
}

void JobSystem::FreeFrame(core::MemoryTag tag) noexcept {
    mFrameHeap.FreeTag(tag);
}

core::FrameAllocator& JobSystem::GetMainFrameAllocator() noexcept {
    return mWorkers[mConfig.mWorkerCount].mFrameAllocator;
}

core::FrameAllocator& JobSystem::GetCurrentFrameAllocator() noexcept {
    if (gCurrentContext != nullptr) {
        return gCurrentContext->mFrameAllocator;
    }
    OYI_ASSERT(std::this_thread::get_id() == mMainThreadId, "no frame allocator on this thread");
    return GetMainFrameAllocator();
}

core::TaggedHeap::Stats JobSystem::GetFrameHeapStats() const noexcept {
    return mFrameHeap.GetStats();
}

JobSystem::Stats JobSystem::GetStats() const noexcept {
    return Stats{.mWorkerCount = mConfig.mWorkerCount, .mInFlightJobs = mInFlight.load(std::memory_order_acquire)};
}

} // namespace oyi::job
